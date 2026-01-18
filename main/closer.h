/**
 * @file closer.h
 * @brief Defer-style resource cleanup utility for C (ESP-IDF compatible)
 *
 * This header defines a "closer" abstraction, allowing registration of
 * cleanup functions that are called in reverse order of registration,
 * similar to Go's "defer".
 *
 * @author garik.djan <garik.djan@gmail.com>
 * @version 0.0.3
 */

#ifndef _CLOSER_H_
#define _CLOSER_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Type of cleanup function to register with a closer.
 */
typedef void (*closer_fn_t)(void);

/**
 * @brief Opaque handle to a closer.
 */
typedef struct closer_t* closer_handle_t;

/**
 * @brief Creates a new closer object.
 *
 * @param[out] out Pointer to a variable to receive the handle.
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_ARG if out is NULL,
 *         ESP_ERR_NO_MEM if memory allocation fails.
 */
esp_err_t closer_create(closer_handle_t* out);

/**
 * @brief Destroys a closer and frees all associated memory.
 *
 * @param h Handle to the closer.
 */
void closer_destroy(closer_handle_t h);

/**
 * @brief Adds a cleanup function to the closer.
 *
 * Functions are called in reverse order of registration when closer_close()
 * or closer_destroy() is called.
 *
 * @param h Handle to the closer.
 * @param fn Cleanup function to add.
 * @return ESP_OK on success,
 *         ESP_ERR_INVALID_ARG if h or fn is NULL,
 *         ESP_ERR_NO_MEM if memory allocation fails.
 */
esp_err_t closer_add(closer_handle_t h, closer_fn_t fn);

/**
 * @brief Calls all registered cleanup functions and clears the list.
 *
 * After calling closer_close(), the closer can still be used to register new functions.
 *
 * @param h Handle to the closer.
 */
void closer_close(closer_handle_t h);

/**
 * @brief Macro to register a cleanup function without checking errors.
 *
 * @param h Handle to the closer.
 * @param fn Cleanup function.
 */
#define CLOSER_DEFER(h, fn)                                                                                            \
    do {                                                                                                               \
        closer_fn_t _fn = (fn);                                                                                        \
        closer_add((h), _fn);                                                                                          \
    } while (0)

/**
 * @brief Macro to register a cleanup function safely.
 *
 * If adding the function fails, the macro logs the error and optionally handles it.
 *
 * @param h Handle to the closer.
 * @param fn Cleanup function.
 * @param on_error Optional: expression to execute if closer_add fails.
 */
#define CLOSER_DEFER_SAFE(h, fn, on_error)                                                                             \
    do {                                                                                                               \
        esp_err_t _err = closer_add((h), (fn));                                                                        \
        if (unlikely(_err != ESP_OK)) {                                                                                \
            on_error;                                                                                                  \
        }                                                                                                              \
    } while (0)

#ifdef CLOSER_IMPLEMENTATION

typedef struct closer_item {
    closer_fn_t fn;
    struct closer_item* next;
} closer_item_t;

struct closer_t {
    closer_item_t* top;
};

esp_err_t closer_create(closer_handle_t* out) {
    if (unlikely(!out))
        return ESP_ERR_INVALID_ARG;

    struct closer_t* c = calloc(1, sizeof(*c));
    if (unlikely(!c))
        return ESP_ERR_NO_MEM;

    *out = c;
    return ESP_OK;
}

void closer_destroy(closer_handle_t h) {
    if (unlikely(!h)) {
        ESP_LOGW(TAG, "closer_destroy called with NULL handle");
        return;
    }

    free(h);
}

esp_err_t closer_add(closer_handle_t h, closer_fn_t fn) {
    if (unlikely(!h || !fn))
        return ESP_ERR_INVALID_ARG;

    closer_item_t* item = malloc(sizeof(*item));
    if (unlikely(!item))
        return ESP_ERR_NO_MEM;

    item->fn = fn;
    item->next = h->top;
    h->top = item;

    return ESP_OK;
}

void closer_close(closer_handle_t h) {
    if (unlikely(!h))
        return;

    closer_item_t* item = h->top;
    while (item) {
        if (item->fn)
            item->fn();

        closer_item_t* tmp = item;
        item = item->next;
        free(tmp);
    }

    h->top = NULL;
}

#endif /* CLOSER_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* _CLOSER_H_ */