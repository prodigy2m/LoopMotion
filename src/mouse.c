#include "main.h"
#include <math.h>

#define MACOS_SWITCH_MOVE_X 10
#define MACOS_SWITCH_MOVE_COUNT 5
#define ACCEL_POINTS 7

/* Check if our upcoming mouse movement would result in having to switch outputs */
enum screen_pos_e is_screen_switch_needed(int position, int offset) {
    if (position + offset < MIN_SCREEN_COORD - global_state.config.jump_threshold)
        return LEFT;

    if (position + offset > MAX_SCREEN_COORD + global_state.config.jump_threshold)
        return RIGHT;

    return NONE;
}

/* Move mouse coordinate 'position' by 'offset', but don't fall off the screen */
int32_t move_and_keep_on_screen(int position, int offset) {
    if (position + offset < MIN_SCREEN_COORD)
        return MIN_SCREEN_COORD;
    else if (position + offset > MAX_SCREEN_COORD)
        return MAX_SCREEN_COORD;
    return position + offset;
}

/* Implement basic mouse acceleration based on actual 2D movement magnitude. */
float calculate_mouse_acceleration_factor(int32_t offset_x, int32_t offset_y) {
    const struct curve {
        int value;
        float factor;
    } acceleration[ACCEL_POINTS] = {
        {2, 1}, {5, 1.1}, {15, 1.4}, {30, 1.9}, {45, 2.6}, {60, 3.4}, {70, 4.0}
    };

    if (offset_x == 0 && offset_y == 0)
        return 1.0;

    if (!global_state.config.enable_acceleration)
        return 1.0;

    const float movement_magnitude = sqrtf((float)(offset_x * offset_x) + (float)(offset_y * offset_y));

    if (movement_magnitude <= acceleration[0].value)
        return acceleration[0].factor;

    if (movement_magnitude >= acceleration[ACCEL_POINTS-1].value)
        return acceleration[ACCEL_POINTS-1].factor;

    const struct curve *lower = NULL;
    const struct curve *upper = NULL;

    for (int i = 0; i < ACCEL_POINTS-1; i++) {
        if (movement_magnitude < acceleration[i + 1].value) {
            lower = &acceleration[i];
            upper = &acceleration[i + 1];
            break;
        }
    }

    if (lower == NULL || upper == NULL)
        return 1.0;

    const float interpolation_pos = (movement_magnitude - lower->value) /
                                  (upper->value - lower->value);

    return lower->factor + interpolation_pos * (upper->factor - lower->factor);
}

/* Returns LEFT if need to jump left, RIGHT if right, NONE otherwise */
enum screen_pos_e update_mouse_position(device_t *state, mouse_values_t *values) {
    output_t *current = &state->config.output[state->active_output];
    uint8_t reduce_speed = state->mouse_zoom ? MOUSE_ZOOM_SCALING_FACTOR : 0;

    float acceleration_factor = calculate_mouse_acceleration_factor(values->move_x, values->move_y);
    int offset_x = round(values->move_x * acceleration_factor * (current->speed_x >> reduce_speed));
    int offset_y = round(values->move_y * acceleration_factor * (current->speed_y >> reduce_speed));

    enum screen_pos_e switch_direction = is_screen_switch_needed(state->pointer_x, offset_x);

    state->pointer_x = move_and_keep_on_screen(state->pointer_x, offset_x);
    state->pointer_y = move_and_keep_on_screen(state->pointer_y, offset_y);

    state->mouse_buttons = values->buttons;

    return switch_direction;
}

/* If we are active output, queue packet to mouse queue, else send them through UART */
void output_mouse_report(mouse_report_t *report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_mouse_report(report, state);
        state->last_activity[BOARD_ROLE] = time_us_64();
    } else {
        queue_packet((uint8_t *)report, MOUSE_REPORT_MSG, MOUSE_REPORT_LENGTH);
    }
}

/* Calculate and return Y coordinate when moving from screen out_from to screen out_to */
int16_t scale_y_coordinate(int screen_from, int screen_to, device_t *state) {
    output_t *from = &state->config.output[screen_from];
    output_t *to = &state->config.output[screen_to];

    int size_to = to->border.bottom - to->border.top;
    int size_from = from->border.bottom - from->border.top;

    if (size_from == size_to)
        return state->pointer_y;

    if (size_from > size_to) {
        return to->border.top + ((size_to * state->pointer_y) / MAX_SCREEN_COORD);
    }

    if (state->pointer_y < from->border.top)
        return MIN_SCREEN_COORD;

    if (state->pointer_y > from->border.bottom)
        return MAX_SCREEN_COORD;

    return ((state->pointer_y - from->border.top) * MAX_SCREEN_COORD) / size_from;
}

void switch_to_another_pc(device_t *state, output_t *output, int output_to, int direction) {
    uint8_t *mouse_park_pos = &state->config.output[state->active_output].mouse_park_pos;

    int16_t mouse_y = (*mouse_park_pos == 0) ? MIN_SCREEN_COORD :
                      (*mouse_park_pos == 1) ? MAX_SCREEN_COORD :
                                               state->pointer_y;

    mouse_report_t hidden_pointer = {.y = mouse_y, .x = MAX_SCREEN_COORD};

    output_mouse_report(&hidden_pointer, state);
    set_active_output(state, output_to);
    state->pointer_x = (direction == LEFT) ? MAX_SCREEN_COORD : MIN_SCREEN_COORD;
    state->pointer_y = scale_y_coordinate(output->number, 1 - output->number, state);
    /* Reset relative_mouse to avoid lingering state */
    state->relative_mouse = (state->config.output[output_to].os == ANDROID);
    // log_debug("switch_to_another_pc: output=%d, relative_mouse=%d", output_to, state->relative_mouse);
}

void switch_virtual_desktop_macos(device_t *state, int direction) {
    mouse_report_t edge_position = {
        .x = (direction == LEFT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD,
        .y = MAX_SCREEN_COORD / 2,
        .mode = ABSOLUTE,
        .buttons = state->mouse_buttons,
    };

    uint16_t move = (direction == LEFT) ? -MACOS_SWITCH_MOVE_X : MACOS_SWITCH_MOVE_X;
    mouse_report_t move_relative_one = {
        .x = move,
        .mode = RELATIVE,
        .buttons = state->mouse_buttons,
    };

    output_mouse_report(&edge_position, state);

    for (int i = 0; i < MACOS_SWITCH_MOVE_COUNT; i++)
        output_mouse_report(&move_relative_one, state);
}

void switch_virtual_desktop_chromeos(device_t *state, int direction) {
    /* Chrome OS: Use larger relative movements to ensure desktop switching */
    int move_x = (direction == LEFT) ? -300 : 300;
    mouse_report_t relative_move = {
        .x = move_x,
        .y = 0,
        .mode = RELATIVE,
        .buttons = state->mouse_buttons,
    };

    /* Send multiple reports to trigger desktop switch */
    for (int i = 0; i < 7; i++)
        output_mouse_report(&relative_move, state);
    // log_debug("switch_virtual_desktop_chromeos: direction=%d, move_x=%d", direction, move_x);
}

void switch_virtual_desktop(device_t *state, output_t *output, int new_index, int direction) {
    switch (output->os) {
        case MACOS:
            switch_virtual_desktop_macos(state, direction);
            break;

        case WINDOWS:
            state->relative_mouse = (new_index > 1);
            break;

        case ANDROID:
            switch_virtual_desktop_chromeos(state, direction);
            break;

        case LINUX:
        case OTHER:
            break;
    }

    state->pointer_x = (direction == RIGHT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD;
    output->screen_index = new_index;
}

void do_screen_switch(device_t *state, int direction) {
    output_t *output = &state->config.output[state->active_output];

    if (state->switch_lock || state->gaming_mode)
        return;

    if (output->pos != direction) {
        if (output->screen_index == 1) {
            if (state->mouse_buttons)
                return;
            switch_to_another_pc(state, output, 1 - state->active_output, direction);
        } else {
            switch_virtual_desktop(state, output, output->screen_index - 1, direction);
        }
    } else if (output->screen_index < output->screen_count) {
        switch_virtual_desktop(state, output, output->screen_index + 1, direction);
    }
}

static inline bool extract_value(bool uses_id, int32_t *dst, report_val_t *src, uint8_t *raw_report, int len) {
    if (uses_id && (*raw_report++ != src->report_id))
        return false;

    *dst = get_report_value(raw_report, len, src);
    return true;
}

void extract_report_values(uint8_t *raw_report, int len, device_t *state, mouse_values_t *values, hid_interface_t *iface) {
    if (iface->protocol == HID_PROTOCOL_BOOT) {
        hid_mouse_report_t *mouse_report = (hid_mouse_report_t *)raw_report;

        values->move_x = mouse_report->x;
        values->move_y = mouse_report->y;
        values->wheel = mouse_report->wheel;
        values->pan = mouse_report->pan;
        values->buttons = mouse_report->buttons;
        return;
    }
    mouse_t *mouse = &iface->mouse;
    bool uses_id = iface->uses_report_id;

    extract_value(uses_id, &values->move_x, &mouse->move_x, raw_report, len);
    extract_value(uses_id, &values->move_y, &mouse->move_y, raw_report, len);
    extract_value(uses_id, &values->wheel, &mouse->wheel, raw_report, len);
    extract_value(uses_id, &values->pan, &mouse->pan, raw_report, len);

    if (!extract_value(uses_id, &values->buttons, &mouse->buttons, raw_report, len)) {
        values->buttons = state->mouse_buttons;
    }
}

mouse_report_t create_mouse_report(device_t *state, mouse_values_t *values, int offset_x, int offset_y) {
    mouse_report_t report = {
        .buttons = values->buttons,
        .x = state->pointer_x,
        .y = state->pointer_y,
        .wheel = values->wheel,
        .pan = values->pan,
        .mode = ABSOLUTE,
    };

    if (state->relative_mouse || state->gaming_mode) {
        report.x = offset_x;
        report.y = offset_y;
        report.mode = RELATIVE;
    }

    // log_debug("create_mouse_report: os=%d, mode=%d, x=%d, y=%d, relative_mouse=%d", 
    //           state->config.output[state->active_output].os, report.mode, report.x, report.y, state->relative_mouse);
    return report;
}

void process_mouse_report(uint8_t *raw_report, int len, uint8_t itf, hid_interface_t *iface) {
    mouse_values_t values = {0};
    device_t *state = &global_state;

    /* Interpret the mouse HID report, extract and save values we need. */
    extract_report_values(raw_report, len, state, &values, iface);

    /* Force relative mode for Chrome OS (ANDROID), absolute for macOS */
    output_t *current = &state->config.output[state->active_output];
    if (current->os == ANDROID) {
        state->relative_mouse = true;
        // log_debug("Chrome OS: relative_mouse=%d", state->relative_mouse);
    } else if (current->os == MACOS) {
        state->relative_mouse = false;
        // log_debug("macOS: relative_mouse=%d", state->relative_mouse);
    }

    /* Calculate movement */
    uint8_t reduce_speed = state->mouse_zoom ? MOUSE_ZOOM_SCALING_FACTOR : 0;
    float acceleration_factor = calculate_mouse_acceleration_factor(values.move_x, values.move_y);
    int offset_x = round(values.move_x * acceleration_factor * (current->speed_x >> reduce_speed));
    int offset_y = round(values.move_y * acceleration_factor * (current->speed_y >> reduce_speed));

    /* Update internal pointer position for screen switching */
    enum screen_pos_e switch_direction = is_screen_switch_needed(state->pointer_x, offset_x);
    state->pointer_x = move_and_keep_on_screen(state->pointer_x, offset_x);
    state->pointer_y = move_and_keep_on_screen(state->pointer_y, offset_y);

    /* Update buttons state */
    state->mouse_buttons = values.buttons;

    /* Create the report for the output PC */
    mouse_report_t report = create_mouse_report(state, &values, offset_x, offset_y);

    /* Move the mouse */
    output_mouse_report(&report, state);

    /* Handle screen switching */
    if (switch_direction != NONE)
        do_screen_switch(state, switch_direction);
}

void process_mouse_queue_task(device_t *state) {
    mouse_report_t report = {0};

    if (!state->tud_connected)
        return;

    if (!queue_try_peek(&state->mouse_queue, &report))
        return;

    if (tud_suspended())
        tud_remote_wakeup();

    if (!tud_hid_n_ready(ITF_NUM_HID))
        return;

    bool succeeded = tud_mouse_report(report.mode, report.buttons, report.x, report.y, report.wheel, report.pan);
    // log_debug("tud_mouse_report: mode=%d, x=%d, y=%d, succeeded=%d", report.mode, report.x, report.y, succeeded);

    if (succeeded)
        queue_try_remove(&state->mouse_queue, &report);
}

void queue_mouse_report(mouse_report_t *report, device_t *state) {
    if (!state->tud_connected)
        return;

    queue_try_add(&state->mouse_queue, report);
}