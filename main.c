#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <fcntl.h>
#include <fcntl.h>


//=======================================================
// ERROR
//=======================================================

#define ERROR_MEMORY_ALLOCATION     (-1000)
#define ERROR_HISTORY_EMPTY         (-1001)
#define ERROR_INDEX_OUT_OF_BOUNDS   (-1002)

#define ERROR_UNKNOWN_COMMAND       (-1020)
#define ERROR_MISSING_COMMAND       (-1021)
#define ERROR_COMMAND_PARAMETERS    (-1022)

//=======================================================
// COMMAND_HISTORY
//=======================================================

typedef enum {
    CHANGE,
    DELETE
} command_type_t;

typedef struct {
    command_type_t type;

    char** old_data;
    size_t* old_data_sizes;
    char** data;
    size_t* data_sizes;

    size_t line_start;
    size_t line_count;
    size_t row_count;
} history_node_t;

typedef struct {
    history_node_t* nodes;
    ssize_t index;
    size_t count;
    size_t capacity;
} history_t;

#define HISTORY_INITIAL_CAPACITY (20)

static void free_node_contents(history_node_t* node) {
    free(node->data);
    free(node->data_sizes);
    free(node->old_data);
    free(node->old_data_sizes);
    node->data = NULL;
    node->data_sizes = NULL;
    node->old_data = NULL;
    node->old_data_sizes = NULL;
}

int command_history_init(history_t* history) {
    history->nodes = (history_node_t*) malloc(sizeof(history_node_t) * HISTORY_INITIAL_CAPACITY);

    history->index = -1;
    history->count = 0;
    history->capacity = HISTORY_INITIAL_CAPACITY;

    return 0;
}
void command_history_free(history_t* history) {
    for (int i = 0; i < history->count; ++i) {
        free_node_contents(history->nodes + i);
    }

    free(history->nodes);

    history->index = -1;
    history->count = 0;
    history->capacity = 0;
}
int command_history_append(history_t* history, const history_node_t* node) {
    if (history->index + 1 >= history->capacity) {
        size_t new_capacity = (size_t) (history->capacity * 2);
        history_node_t* new_nodes = (history_node_t*) realloc(history->nodes, sizeof(history_node_t) * new_capacity);
        if (NULL == new_nodes) {
            return ERROR_MEMORY_ALLOCATION;
        }

        history->nodes = new_nodes;
        history->capacity = new_capacity;
    }

    ++history->index;
    if (history->index < history->count) {
        // override nodes
        for (int i = history->index; i < history->count; ++i) {
            free_node_contents(history->nodes + i);
        }
    }

    history_node_t* node_in_history = history->nodes + history->index;
    memcpy(node_in_history, node, sizeof(history_node_t));
    history->count = history->index + 1;

    return 0;
}
int command_history_forward(history_t* history, history_node_t* node) {
    if (history->index + 1 >= history->count) {
        return ERROR_HISTORY_EMPTY;
    }

    ++history->index;
    history_node_t* node_in_history = history->nodes + history->index;
    memcpy(node, node_in_history, sizeof(history_node_t));

    return 0;
}
int command_history_back(history_t* history, history_node_t* node) {
    if (history->index < -1) {
        return ERROR_HISTORY_EMPTY;
    }
    if (-1 == history->index) {
        memset(node, 0, sizeof(history_node_t));
        return 0;
    }

    history_node_t* node_in_history = history->nodes + history->index;
    memcpy(node, node_in_history, sizeof(history_node_t));
    --history->index;

    return 0;
}

int command_history_update(history_t* history, history_node_t* node) {
    history_node_t* node_in_history = history->nodes + history->index + 1;
    memcpy(node_in_history, node, sizeof(history_node_t));

    return 0;
}

//=======================================================
// EDITOR
//=======================================================

typedef struct {
    char** rows;
    size_t* sizes;
    size_t row_count;
    size_t row_capacity;

    ssize_t delayed_history_change_count;

    history_t history;
} editor_t;

#define EDITOR_INITIAL_CAPACITY (20)
#define MAX_LINE_SIZE (1024)

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define NORMALIZE_LINE(l) \
    (l) = MIN((l), editor->row_count - 1)
#define ROW(ed, idx) ((ed)->rows[idx])

static int expand_buffers(editor_t* editor, size_t needed_size) {
    size_t new_capacity = (size_t) (needed_size * 2);
    char** rows = (char**) realloc(editor->rows, sizeof(char*) * new_capacity);
    size_t* sizes = (size_t*) realloc(editor->sizes, sizeof(size_t) * new_capacity);

    for (int i = editor->row_capacity; i < new_capacity; ++i) {
        sizes[i] = 0;
    }


    editor->rows = rows;
    editor->sizes = sizes;
    editor->row_capacity = new_capacity;

    return 0;
}
static int copy_lines(editor_t* editor, size_t line_start, size_t lines_count,
                      char*** buffer, size_t** sizes) {
    if (0 == lines_count) {
        *buffer = NULL;
        *sizes = NULL;
        return 0;
    }

    size_t* data_sizes = (size_t*) malloc(sizeof(size_t) * lines_count);
    char** data = (char**) malloc(sizeof(char*) * lines_count);


    for (int i = line_start; i < line_start + lines_count; ++i) {
        data_sizes[i] = editor->sizes[line_start + i];
        data[i] = editor->rows[i];
    }

    *buffer = data;
    *sizes = data_sizes;
    return 0;
}
static int change_lines(editor_t* editor, size_t line_start, size_t lines_count,
                        char** data, const size_t* sizes) {
    for (int i = 0; i < lines_count; ++i) {
        editor->sizes[line_start + i] = sizes[i];
        editor->rows[line_start + i] = data[i];
    }

    if (line_start + lines_count > editor->row_count) {
        editor->row_count = line_start + lines_count;
    }

    return 0;
}
static int change_lines2(editor_t* editor, size_t line_start, size_t lines_count,
                        char* data, const size_t* sizes) {
    size_t offset = 0;
    for (int i = 0; i < lines_count; ++i) {
        editor->sizes[line_start + i] = sizes[i];
        editor->rows[line_start + i] = data + offset;
        offset += sizes[i] + 1;
    }

    if (line_start + lines_count > editor->row_count) {
        editor->row_count = line_start + lines_count;
    }

    return 0;
}
static int delete_lines(editor_t* editor, size_t line_start, size_t lines_count) {
    size_t line_end = line_start + lines_count;

    for (int i = 0; i < editor->row_count - line_start; ++i) {
        if (line_end + i < editor->row_count) {
            editor->sizes[line_start + i] = editor->sizes[line_end + i];
            editor->rows[line_start + i] = editor->rows[line_end + i];
            /*memcpy(ROW(editor, (line_start + i)),
                   ROW(editor, (line_end + i)),
                   editor->sizes[line_start + i]);*/

            //ROW(editor, (line_start + i))[editor->sizes[line_start + i]] = '\0';
        }
    }

    if (lines_count >= editor->row_count) {
        editor->row_count = 0;
    } else {
        editor->row_count -= lines_count;
    }

    return 0;
}

int editor_init(editor_t* editor) {
    char** rows = (char**) malloc(sizeof(char*) * EDITOR_INITIAL_CAPACITY);
    size_t* sizes = (size_t*) malloc(sizeof(size_t) * EDITOR_INITIAL_CAPACITY);



    for (int i = 0; i < EDITOR_INITIAL_CAPACITY; ++i) {
        sizes[i] = 0;
    }

    command_history_init(&editor->history);

    editor->rows = rows;
    editor->sizes = sizes;
    editor->row_count = 0;
    editor->row_capacity = EDITOR_INITIAL_CAPACITY;
    editor->delayed_history_change_count = 0;

    return 0;
}
void editor_free(editor_t* editor) {
    free(editor->rows);
    free(editor->sizes);

    editor->row_count = 0;
    editor->row_capacity = 0;

    command_history_free(&editor->history);
}

int editor_change(editor_t* editor,
                  size_t line_start, size_t lines_count,
                  char* input, size_t input_size, size_t* input_sizes) {
    if (line_start > editor->row_count) {
        // error, not linked to existing rows
        return ERROR_INDEX_OUT_OF_BOUNDS;
    }

    int result;

    if (line_start + lines_count >= editor->row_capacity) {
        result = expand_buffers(editor, line_start + lines_count);
        if (result) {
            return result;
        }
    }

    history_node_t history = {
            .type = CHANGE,
            .line_start = line_start,
            .line_count = lines_count,
            .row_count = editor->row_count,
            .old_data = NULL,
            .old_data_sizes = NULL,
            .data = NULL,
            .data_sizes = NULL
    };

    char* *old_data;
    size_t* old_data_sizes;
    size_t buffer_lines_count = lines_count;
    if(line_start + lines_count > editor->row_count) {
        buffer_lines_count = editor->row_count - line_start;
    }

    copy_lines(editor, line_start, buffer_lines_count, &old_data, &old_data_sizes);
    change_lines2(editor, line_start, lines_count, input, input_sizes);

    history.old_data = old_data;
    history.old_data_sizes = old_data_sizes;
    command_history_append(&editor->history, &history);

    return 0;
}
int editor_delete(editor_t* editor,
                  size_t line_start, size_t lines_count) {
    if (0 == editor->row_count) {
        history_node_t history = {
                .type = DELETE,
                .line_start = line_start,
                .line_count = lines_count,
                .row_count = 0,
                .old_data = NULL,
                .old_data_sizes = NULL,
                .data = NULL,
                .data_sizes = NULL
        };

        return command_history_append(&editor->history, &history);
    }

    if(line_start + lines_count >= editor->row_count) {
        lines_count = editor->row_count - line_start;
    }

    history_node_t history = {
            .type = DELETE,
            .line_start = line_start,
            .line_count = lines_count,
            .row_count = editor->row_count,
            .old_data = NULL,
            .old_data_sizes = NULL,
            .data = NULL,
            .data_sizes = NULL
    };

    char** old_data;
    size_t* old_data_sizes;
    copy_lines(editor, line_start, lines_count, &old_data, &old_data_sizes);
    delete_lines(editor, line_start, lines_count);

    history.old_data = old_data;
    history.old_data_sizes = old_data_sizes;
    command_history_append(&editor->history, &history);

    return 0;
}
int editor_undo(editor_t* editor, size_t count) {
    int result;
    history_node_t node;

    while (count-- > 0) {
        result = command_history_back(&editor->history, &node);
        if (result) {
            return 0;
        }

        switch (node.type) {
            case CHANGE: {
                if (NULL == node.data) {
                    char** old_data;
                    size_t* old_data_sizes;
                    copy_lines(editor, node.line_start, node.line_count, &old_data, &old_data_sizes);

                    node.data = old_data;
                    node.data_sizes = old_data_sizes;

                    command_history_update(&editor->history, &node);
                }

                if (NULL == node.old_data) {
                    editor->row_count = node.row_count;
                    continue;
                }

                if (node.line_start + node.line_count >= node.row_count) {
                    change_lines(editor,
                                          node.line_start, node.row_count - node.line_start,
                                          node.old_data, node.old_data_sizes);
                } else {
                    change_lines(editor,
                                          node.line_start, node.line_count,
                                          node.old_data, node.old_data_sizes);
                }
                break;
            }
            case DELETE: {
                size_t line_end = node.line_start + node.line_count;
                for (int i = editor->row_count - node.line_start - 1; i >= 0; --i) {
                    editor->sizes[line_end + i] = editor->sizes[node.line_start + i];
                    editor->rows[line_end + i] = editor->rows[node.line_start + i];
                    /*memcpy(ROW(editor, line_end + i),
                           ROW(editor, node.line_start + i),
                           editor->sizes[line_end + i]);*/
                    //ROW(editor, line_end + i)[editor->sizes[line_end + i]] = '\0';
                }

                if (NULL == node.old_data) {
                    editor->row_count = node.row_count;
                    continue;
                }

                change_lines(editor,
                                      node.line_start, node.line_count,
                                      node.old_data, node.old_data_sizes);
                break;
            }
        }

        editor->row_count = node.row_count;
    }

    return 0;
}
int editor_redo(editor_t* editor, size_t count) {
    int result;
    history_node_t node;

    while (count-- > 0) {
        result = command_history_forward(&editor->history, &node);
        if (result) {
            return 0;
        }

        switch (node.type) {
            case CHANGE:
                change_lines(editor,
                                      node.line_start, node.line_count,
                                      node.data, node.data_sizes);
                break;
            case DELETE: {
                if (editor->row_count > 0) {
                    delete_lines(editor,
                                          node.line_start, node.line_count);
                }
                break;
            }
        }
    }

    return 0;
}

const char EMPTY_LINE_BUFFER[] = ".\n";
const char NEW_LINE_BUFFER[] = "\n";

int editor_print(editor_t* editor,
                 size_t line_start, size_t lines_count, FILE* stream) {
    for (int i = line_start; i < line_start + lines_count; ++i) {
        if (i >= editor->row_count) {
            // have a huge buffer and print some of it depending on how many empty lines
            fwrite(EMPTY_LINE_BUFFER, 1, 2, stream);
        } else {
            fwrite(editor->rows[i], 1, editor->sizes[i], stream);
            fwrite(NEW_LINE_BUFFER, 1, 1, stream);
        }
    }

    return 0;
}

int editor_change_history(editor_t* editor) {
    int result = 0;
    if (editor->delayed_history_change_count > 0) {
        result = editor_redo(editor, editor->delayed_history_change_count);
    } else if (editor->delayed_history_change_count < 0) {
        result = editor_undo(editor, -editor->delayed_history_change_count);
    }

    editor->delayed_history_change_count = 0;
    return result;
}

//=======================================================
// PARSING
//=======================================================

#define COMMAND_CHANGE ('c')
#define COMMAND_DELETE ('d')
#define COMMAND_UNDO ('u')
#define COMMAND_REDO ('r')
#define COMMAND_PRINT ('p')
#define COMMAND_EXIT ('q')


int parse_command_2_params(char *input, size_t command_char_index,
                           int *first, int *second) {
    char* index_sep = memchr(input, ',', 20);

    char* end = input + command_char_index;
    *first = strtol(input, &index_sep, 10);

    *second = strtol(index_sep + 1, &end, 10);

    return 0;
}

int parse_command_1_param(char *input, size_t command_char_index, int *first) {
    char* end = input + command_char_index;
    *first = strtol(input, &end, 10);

    return 0;
}

int parse_command(char* input, size_t input_size,
                  char* command_char, bool* exit, bool* read_lines,
                  int* first_index, int* second_index) {
    int result = 0;
    *read_lines = false;
    *exit = false;

    int command_char_index = input_size - 1;
    *command_char = input[command_char_index];

    switch (input[command_char_index]) {
        case COMMAND_CHANGE:
            result = parse_command_2_params(input, command_char_index,
                                            first_index, second_index);
            *read_lines = true;
            break;
        case COMMAND_DELETE:
            result = parse_command_2_params(input, command_char_index,
                                            first_index, second_index);
            break;
        case COMMAND_UNDO:
            result = parse_command_1_param(input, command_char_index, first_index);
            break;
        case COMMAND_REDO:
            result = parse_command_1_param(input, command_char_index, first_index);
            break;
        case COMMAND_PRINT:
            result = parse_command_2_params(input, command_char_index,
                                            first_index, second_index);
            break;
        case COMMAND_EXIT:
            *exit = true;
            break;
        default:
            result = ERROR_UNKNOWN_COMMAND;
            break;
    }

    return result;
}

int do_command(editor_t* editor, char* input, size_t lines_count, char command_char,
               int first_index, int second_index, size_t input_size, size_t* input_sizes) {
    int result;
    switch (command_char) {
        case COMMAND_CHANGE:
            // execute history change
            editor_change_history(editor);
            result = editor_change(editor, first_index - 1, lines_count,
                                   input, input_size, input_sizes);
            return result;
        case COMMAND_DELETE: {
            // execute history change
            editor_change_history(editor);
            lines_count = second_index - first_index + 1;
            result = editor_delete(editor, first_index - 1, lines_count);
            return result;
        }
        case COMMAND_UNDO: {
            ssize_t new_index = editor->history.index + editor->delayed_history_change_count - first_index;
            //editor->delayed_history_change_count -= first_index;
            if (new_index >= -1) {
                editor->delayed_history_change_count -= first_index;
            } else {
                editor->delayed_history_change_count = -editor->history.index - 1;
            }
            //printf("%ld\n", editor->delayed_history_change_count);
            return 0;//editor_undo(editor, first_index);
        }
        case COMMAND_REDO: {
            ssize_t new_index = editor->history.index + editor->delayed_history_change_count + first_index;
            if (new_index < editor->history.count) {
                editor->delayed_history_change_count += first_index;
            } else {
                editor->delayed_history_change_count = editor->history.count - editor->history.index - 1;
            }
            //printf("%ld\n", editor->delayed_history_change_count);
            return 0;//editor_undo(editor, first_index);
        }
        case COMMAND_PRINT: {
            if (0 == first_index || 0 == second_index) {
                fwrite(EMPTY_LINE_BUFFER, 1, 2, stdout);
                return 0;
            } else {
                // execute history change
                editor_change_history(editor);
                lines_count = second_index - first_index + 1;
                return editor_print(editor, first_index - 1, lines_count, stdout);
            }
        }
    }

    return 1;
}

//=======================================================
// MAIN
//=======================================================

#define INPUT_BUFFER_SIZE (1026)
#define LINES_BUFFER_SIZE (4096 * 15)

//#define TIME_CHECK

#ifdef TIME_CHECK
#include <time.h>
#endif

int main() {
#ifdef TIME_CHECK
    clock_t begin = clock();
#endif

    char* lines_buffer = (char*) malloc(LINES_BUFFER_SIZE * 10);
    size_t lines_buffer_offset = 0;
    size_t lines_buffer_size = LINES_BUFFER_SIZE * 10;

    size_t* input_sizes = malloc(sizeof(size_t) * 100);
    size_t input_sizes_capacity = 100;

    int result;
    int lines_count = 0;

    int first_index;
    int second_index;
    bool read_lines;
    bool do_exit;
    char command_char;
    char* off;

    //setvbuf(stdin, NULL, _IONBF, 0);

    fread(lines_buffer, 1, 1, stdin);
    lines_buffer_offset += 1;

    int flags = fcntl(0, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(0, F_SETFL, flags);

    while((result = fread(lines_buffer + lines_buffer_offset, 1, LINES_BUFFER_SIZE, stdin)) > 0) {
        lines_buffer_offset += result;
        if (lines_buffer_offset >= lines_buffer_size) {
            lines_buffer = (char*) realloc(lines_buffer, lines_buffer_size * 2);
            lines_buffer_size = lines_buffer_size * 2;
        }
    }


    editor_t editor;
    editor_init(&editor);

    char* orig_lines_buffer = lines_buffer;

    while (1) {
        lines_count = 0;

        off = memchr(lines_buffer, '\n', 30);
        result = off - lines_buffer;

        parse_command(lines_buffer, result, &command_char, &do_exit,
                               &read_lines, &first_index, &second_index);

        if (do_exit) {
            break;
        }
        if(!read_lines) {
            do_command(&editor, lines_buffer, lines_count, command_char,
                                first_index, second_index, 0, NULL);

            lines_buffer = off + 1;
            continue;
        }
        lines_buffer = off + 1;

        lines_count = 0;
        lines_buffer_offset = 0;
        char* data_buffer = lines_buffer;

        while (1) {
            lines_buffer = off + 1;
            off = memchr(lines_buffer, '\n', 1030);
            result = off - lines_buffer;

            if (result <= 0) {
                continue;
            } else if (1 == result && '.' == lines_buffer[0]) {
                data_buffer[lines_buffer_offset] = '\0';
                break;
            } else {
                if (lines_count >= input_sizes_capacity) {
                    input_sizes_capacity = input_sizes_capacity * 2;
                    input_sizes = (size_t*) realloc(input_sizes, sizeof(size_t) * input_sizes_capacity);
                    if (NULL == input_sizes) {
                        return 1;
                    }
                }

                lines_buffer_offset += result + 1;
                input_sizes[lines_count] = result;
                lines_count++;
            }
        }
        lines_buffer = off + 1;

        if (lines_count != second_index - first_index + 1) {
            continue;
        }

        // execute command
        do_command(&editor, data_buffer, lines_count, command_char,
                            first_index, second_index, lines_buffer_offset,
                            input_sizes);
    }

    free(orig_lines_buffer);
    free(input_sizes);
    editor_free(&editor);

#ifdef TIME_CHECK
    clock_t end = clock();
    printf("%f\n", (double) (end - begin) / CLOCKS_PER_SEC);
#endif
    return 0;
}


