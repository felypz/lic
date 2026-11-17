#ifndef UI_H
#define UI_H

#include "lua_api.h"
#include "state.h"
#include <stdbool.h>
#include <stddef.h>

#define UI_INPUT_MAX    1024
#define UI_COMP_MAX     128
#define UI_COMP_STR_LEN 64

typedef enum {
	UI_ACTION_NONE,
	UI_ACTION_SUBMIT,
	UI_ACTION_NEXT_BUF,
	UI_ACTION_PREV_BUF,
	UI_ACTION_SWITCH_BUF,
	UI_ACTION_QUIT
} UiAction;

typedef struct {
	int rows;
	int cols;
	char input[UI_INPUT_MAX];
	size_t input_len;
	size_t cursor;
	size_t scroll_offset;
	size_t target_buf_idx;
	char prompt_label[64];
	bool dirty;

	bool comp_active;
	size_t comp_start;
	size_t comp_end;
	char comp_matches[UI_COMP_MAX][UI_COMP_STR_LEN];
	size_t comp_count;
	size_t comp_idx;
} UiState;

bool ui_init(UiState *ui);
void ui_shutdown(void);
void ui_update_size(UiState *ui);
void ui_render(UiState *ui, const AppState *st, const LicTheme *theme);
UiAction ui_handle_stdin(UiState *ui, const AppState *st, struct LuaEngine *lua, char *out_line, size_t max_len);

#endif
