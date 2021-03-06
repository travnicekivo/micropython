/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2014 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include "ets_sys.h"
#include "etshal.h"
#include "uart.h"
#include "esp_mphal.h"
#include "user_interface.h"
#include "ets_alt_task.h"
#include "py/obj.h"
#include "py/mpstate.h"
#include "extmod/misc.h"
#include "lib/utils/pyexec.h"

extern void ets_wdt_disable(void);
extern void wdt_feed(void);
extern void ets_delay_us();

STATIC byte input_buf_array[256];
ringbuf_t input_buf = {input_buf_array, sizeof(input_buf_array)};
void mp_hal_debug_tx_strn_cooked(void *env, const char *str, uint32_t len);
const mp_print_t mp_debug_print = {NULL, mp_hal_debug_tx_strn_cooked};

void mp_hal_init(void) {
    ets_wdt_disable(); // it's a pain while developing
    mp_hal_rtc_init();
    uart_init(UART_BIT_RATE_115200, UART_BIT_RATE_115200);
}

void mp_hal_feed_watchdog(void) {
    //ets_wdt_disable(); // it's a pain while developing
    //WRITE_PERI_REG(0x60000914, 0x73);
    //wdt_feed(); // might also work
}

void mp_hal_delay_us(uint32_t us) {
    uint32_t start = system_get_time();
    while (system_get_time() - start < us) {
        ets_event_poll();
    }
}

int mp_hal_stdin_rx_chr(void) {
    for (;;) {
        int c = ringbuf_get(&input_buf);
        if (c != -1) {
            return c;
        }
        mp_hal_delay_us(1);
        mp_hal_feed_watchdog();
    }
}

void mp_hal_stdout_tx_char(char c) {
    uart_tx_one_char(UART0, c);
    mp_uos_dupterm_tx_strn(&c, 1);
}

#if 0
void mp_hal_debug_str(const char *str) {
    while (*str) {
        uart_tx_one_char(UART0, *str++);
    }
    uart_flush(UART0);
}
#endif

void mp_hal_stdout_tx_str(const char *str) {
    while (*str) {
        mp_hal_stdout_tx_char(*str++);
    }
}

void mp_hal_stdout_tx_strn(const char *str, uint32_t len) {
    while (len--) {
        mp_hal_stdout_tx_char(*str++);
    }
}

void mp_hal_stdout_tx_strn_cooked(const char *str, uint32_t len) {
    while (len--) {
        if (*str == '\n') {
            mp_hal_stdout_tx_char('\r');
        }
        mp_hal_stdout_tx_char(*str++);
    }
}

void mp_hal_debug_tx_strn_cooked(void *env, const char *str, uint32_t len) {
    (void)env;
    while (len--) {
        if (*str == '\n') {
            uart_tx_one_char(UART0, '\r');
        }
        uart_tx_one_char(UART0, *str++);
    }
}

uint32_t mp_hal_ticks_ms(void) {
    return system_get_time() / 1000;
}

uint32_t mp_hal_ticks_us(void) {
    return system_get_time();
}

void mp_hal_delay_ms(uint32_t delay) {
    mp_hal_delay_us(delay * 1000);
}

void mp_hal_set_interrupt_char(int c) {
    if (c != -1) {
        mp_obj_exception_clear_traceback(MP_STATE_PORT(mp_kbd_exception));
    }
    extern int interrupt_char;
    interrupt_char = c;
}

void ets_event_poll(void) {
    ets_loop_iter();
    if (MP_STATE_VM(mp_pending_exception) != NULL) {
        mp_obj_t obj = MP_STATE_VM(mp_pending_exception);
        MP_STATE_VM(mp_pending_exception) = MP_OBJ_NULL;
        nlr_raise(obj);
    }
}

void __assert_func(const char *file, int line, const char *func, const char *expr) {
    printf("assert:%s:%d:%s: %s\n", file, line, func, expr);
    nlr_raise(mp_obj_new_exception_msg(&mp_type_AssertionError,
        "C-level assert"));
}

void mp_hal_signal_input(void) {
    #if MICROPY_REPL_EVENT_DRIVEN
    system_os_post(UART_TASK_ID, 0, 0);
    #endif
}

static int call_dupterm_read(void) {
    if (MP_STATE_PORT(term_obj) == NULL) {
        return -1;
    }

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t read_m[3];
        mp_load_method(MP_STATE_PORT(term_obj), MP_QSTR_read, read_m);
        read_m[2] = MP_OBJ_NEW_SMALL_INT(1);
        mp_obj_t res = mp_call_method_n_kw(1, 0, read_m);
        if (res == mp_const_none) {
            return -2;
        }
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(res, &bufinfo, MP_BUFFER_READ);
        if (bufinfo.len == 0) {
            MP_STATE_PORT(term_obj) = NULL;
            mp_printf(&mp_plat_print, "dupterm: EOF received, deactivating\n");
            return -1;
        }
        nlr_pop();
        return *(byte*)bufinfo.buf;
    } else {
        MP_STATE_PORT(term_obj) = NULL;
        mp_printf(&mp_plat_print, "dupterm: Exception in read() method, deactivating: ");
        mp_obj_print_exception(&mp_plat_print, nlr.ret_val);
    }

    return -1;
}

STATIC void dupterm_task_handler(os_event_t *evt) {
    static byte lock;
    if (lock) {
        return;
    }
    lock = 1;
    while (1) {
        int c = call_dupterm_read();
        if (c < 0) {
            break;
        }
        ringbuf_put(&input_buf, c);
    }
    mp_hal_signal_input();
    lock = 0;
}

STATIC os_event_t dupterm_evt_queue[4];

void dupterm_task_init() {
    system_os_task(dupterm_task_handler, DUPTERM_TASK_ID, dupterm_evt_queue, MP_ARRAY_SIZE(dupterm_evt_queue));
}

void mp_hal_signal_dupterm_input(void) {
    system_os_post(DUPTERM_TASK_ID, 0, 0);
}
