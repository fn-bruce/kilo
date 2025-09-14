/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editor_key {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

enum editor_highlight {
  HL_NORMAL = 0,
  HL_NUMBER,
  HL_MATCH
};

/*** data ***/

typedef struct editor_row {
  int size;
  int render_size;
  char *chars;
  char *render;
  unsigned char *highlight;
} editor_row;

struct editor_config {
  int cursor_x;
  int cursor_y;
  int render_x;
  int row_offset;
  int col_offset;
  int screen_rows;
  int screen_cols;
  int num_rows;
  editor_row *row;
  int dirty;
  char *filename;
  char status_msg[80];
  time_t status_msg_time;
  struct termios orig_termios;
};

struct editor_config E;

/*** prototypes ***/

void editor_set_status_message(const char *fmt, ...);
void editor_refresh_screen(void);
char* editor_prompt(char *prompt, void (*callback)(char*, int));

/*** terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);
  exit(1);
}

void disable_raw_mode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

void enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
    die("tcgetattr");
  }
  atexit(disable_raw_mode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

int editor_read_key(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) {
      return '\x1b';
    }

    if (read(STDIN_FILENO, &seq[1], 1) != 1) {
      return '\x1b';
    }

    // 4 down
    // 21 up
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) {
          return '\x1b';
        }
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1':
              return HOME_KEY;
            case '3':
              return DEL_KEY;
            case '4':
              return END_KEY;
            case '5':
              return PAGE_UP;
            case '6':
              return PAGE_DOWN;
            case '7':
              return HOME_KEY;
            case '8':
              return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A':
            return ARROW_UP;
          case 'B':
            return ARROW_DOWN;
          case 'C':
            return ARROW_RIGHT;
          case 'D':
            return ARROW_LEFT;
          case 'H':
            return HOME_KEY;
          case 'F':
            return END_KEY;
        }
      }
    }

    return '\x1b';
  }

  return c;
}

int get_cursor_position(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }

    if (buf[i] == 'R') {
      break;
    }

    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }

  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }

  return 0;
}

int get_window_size(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    return get_cursor_position(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** syntax highlighting ***/

void editor_update_syntax(editor_row *row) {
  row->highlight = realloc(row->highlight, row->size);
  memset(row->highlight, HL_NORMAL, row->size);

  int i;
  for (i = 0; i < row->size; i++) {
    if (isdigit(row->render[i])) {
      row->highlight[i] = HL_NUMBER;
    }
  }
}

int editor_syntax_to_color(int highlight) {
  switch (highlight) {
    case HL_NUMBER:
      return 31;
    case HL_MATCH:
      return 34;
    default:
      return 37;
  }
}

/*** row operations ***/

int editor_row_cursor_x_to_render_x(editor_row *row, int cursor_x) {
  int render_x = 0;
  int j;
  for (j = 0; j < cursor_x; j++) {
    if (row->chars[j] == '\t') {
      render_x += (KILO_TAB_STOP - 1) - (render_x % KILO_TAB_STOP);
    }
    render_x++;
  }
  return render_x;
}

int eidtor_row_render_x_to_cursor_x(editor_row *row, int render_x) {
  int curr_render_x = 0;
  int cursor_x;
  for (cursor_x = 0; cursor_x < row->size; cursor_x++) {
    if (row->chars[cursor_x] == '\t') {
      curr_render_x += (KILO_TAB_STOP - 1) - (curr_render_x % KILO_TAB_STOP);
    }
    curr_render_x++;

    if (curr_render_x > render_x) {
      return cursor_x;
    }
  }

  return cursor_x;
}

void editor_update_row(editor_row *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      tabs++;
    }
  }

  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) {
        row->render[idx++] = ' ';
      }
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->render_size = idx;

  editor_update_syntax(row);
}

void editor_insert_row(int at, char *s, size_t len) {
  if (at < 0 || at > E.num_rows) {
    return;
  }

  E.row = realloc(E.row, sizeof(editor_row) * (E.num_rows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(editor_row) * (E.num_rows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].render_size = 0;
  E.row[at].render = NULL;
  E.row[at].highlight = NULL;
  editor_update_row(&E.row[at]);

  E.num_rows++;
  E.dirty++;
}

void editor_free_row(editor_row *row) {
  free(row->render);
  free(row->chars);
  free(row->highlight);
}

void editor_del_row(int at) {
  if (at < 0 || at >= E.num_rows) {
    return;
  }

  editor_free_row(&E.row[at]);
  memmove(
    &E.row[at],
    &E.row[at + 1],
    sizeof(editor_row) * (E.num_rows - at - 1));
  E.num_rows--;
  E.dirty++;
}

void editor_row_insert_char(editor_row *row, int at, int c) {
  if (at < 0 || at > row->size) {
    at = row->size;
  }
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editor_update_row(row);
  E.dirty++;
}

void editor_row_append_string(editor_row *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editor_update_row(row);
  E.dirty++;
}

void editor_row_del_char(editor_row *row, int at) {
  if (at < 0 || at >= row->size) {
    return;
  }
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editor_update_row(row);
  E.dirty++;
}

/*** editor operations ***/

void editor_insert_char(int c) {
  if (E.cursor_y == E.num_rows) {
    editor_insert_row(E.num_rows, "", 0);
  }
  editor_row_insert_char(&E.row[E.cursor_y], E.cursor_x, c);
  E.cursor_x++;
}

void editor_insert_newline(void) {
  if (E.cursor_x == 0) {
    editor_insert_row(E.cursor_y, "", 0);
  } else {
    editor_row *row = &E.row[E.cursor_y];
    editor_insert_row(
      E.cursor_y + 1,
      &row->chars[E.cursor_x],
      row->size - E.cursor_x);
    row = &E.row[E.cursor_y];
    row->size = E.cursor_x;
    row->chars[row->size] = '\0';
    editor_update_row(row);
  }
  E.cursor_y++;
  E.cursor_x = 0;
}

void editor_del_char(void) {
  if (E.cursor_y == E.num_rows) {
    return;
  }
  if (E.cursor_x == 0 && E.cursor_y == 0) {
    return;
  }

  editor_row *row = &E.row[E.cursor_y];
  if (E.cursor_x > 0) {
    editor_row_del_char(row, E.cursor_x - 1);
    E.cursor_x--;
  } else {
    E.cursor_x = E.row[E.cursor_y - 1].size;
    editor_row_append_string(&E.row[E.cursor_y - 1], row->chars, row->size);
    editor_del_row(E.cursor_y);
    E.cursor_y--;
  }
}

/*** file i/o ***/

char *editor_rows_to_string(int *buffer_length) {
  int total_length = 0;
  int j;
  for (j = 0; j < E.num_rows; j++) {
    total_length += E.row[j].size + 1;
  }
  *buffer_length = total_length;

  char *buffer = malloc(total_length);
  char *p = buffer;
  for (j = 0; j < E.num_rows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buffer;
}

void editor_open(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) {
    die("fopen");
  }

  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_len;
  while ((line_len = getline(&line, &line_cap, fp)) != -1) {
    while (line_len > 0 && (line[line_len - 1] == '\n' ||
                            line[line_len - 1] == '\r')) {
      line_len --;
    }
    editor_insert_row(E.num_rows, line, line_len);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editor_save(void) {
  if (E.filename == NULL) {
    E.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
    if (E.filename == NULL) {
      editor_set_status_message("Save aborted");
      return;
    }
  }

  int length;
  char *buffer = editor_rows_to_string(&length);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, length) != -1) {
      if (write(fd, buffer, length) == length) {
        close(fd);
        free(buffer);
        E.dirty = 0;
        editor_set_status_message("%d bytes written to disk", length);
        return;
      }
    }
    close(fd);
  }
  free(buffer);
  editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editor_find_callback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1) {
    direction = 1;
  }

  int current = last_match;
  int i;
  for (i = 0; i < E.num_rows; i++) {
    current += direction;
    if (current == -1) {
      current = E.num_rows - 1;
    } else if (current == E.num_rows) {
      current = 0;
    }

    editor_row *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      E.cursor_y = current;
      E.cursor_x = editor_row_cursor_x_to_render_x(row, match - row->render);
      E.row_offset = E.num_rows;

      memset(&row->highlight[match - row->render], HL_MATCH, strlen(query));
      break;
    }
  }
}

void editor_find(void) {
  int saved_cursor_x = E.cursor_x;
  int saved_cursor_y = E.cursor_y;
  int saved_col_offset = E.col_offset;
  int saved_row_offset = E.row_offset;

  char *query = editor_prompt(
    "Search: %s (Use ESC/Arrows/Enter)",
    editor_find_callback);

  if (query) {
    free(query);
  } else {
    E.cursor_x = saved_cursor_x;
    E.cursor_y = saved_cursor_y;
    E.col_offset = saved_col_offset;
    E.row_offset = saved_row_offset;
  }
}

/*** append buffer ***/

struct append_buffer {
  char *buffer;
  int length;
};

#define APPEND_BUFFER_INIT {NULL, 0}

void append_buffer_append(
  struct append_buffer *append_buffer,
  const char *str,
  int length) {
  char *new = realloc(append_buffer->buffer, append_buffer->length + length);
  if (new == NULL) {
    return;
  }

  memcpy(&new[append_buffer->length], str, length);
  append_buffer->buffer = new;
  append_buffer->length += length;
}

void append_buffer_free(struct append_buffer *append_buffer) {
  free(append_buffer->buffer);
}

/*** output ***/

void editor_scroll(void) {
  E.render_x = E.cursor_x;
  if (E.cursor_y < E.num_rows) {
    E.render_x = editor_row_cursor_x_to_render_x(
      &E.row[E.cursor_y],
      E.cursor_x);
  }

  if (E.cursor_y < E.row_offset) {
    E.row_offset = E.cursor_y;
  }
  if (E.cursor_y >= E.row_offset + E.screen_rows) {
    E.row_offset = E.cursor_y - E.screen_rows + 1;
  }
  if (E.render_x < E.col_offset) {
    E.col_offset = E.render_x;
  }
  if (E.render_x >= E.col_offset + E.screen_cols) {
    E.col_offset = E.render_x - E.screen_cols + 1;
  }
}

void editor_draw_rows(struct append_buffer *append_buffer) {
  int y;
  for (y = 0; y < E.screen_rows; y++) {
    int file_row = y + E.row_offset;
    if (file_row >= E.num_rows) {
      if (E.num_rows == 0 && y == E.screen_rows / 3) {
        char welcome[80];
        int welcome_length = snprintf(
          welcome,
          sizeof(welcome),
          "Kilo editor -- version %s",
          KILO_VERSION);
        if (welcome_length > E.screen_cols) {
          welcome_length = E.screen_cols;
        }

        int padding = (E.screen_cols - welcome_length) / 2;
        if (padding) {
          append_buffer_append(append_buffer, "~", 1);
          padding--;
        }

        while (padding--) {
          append_buffer_append(append_buffer, " ", 1);
        }

        append_buffer_append(append_buffer, welcome, welcome_length);
      } else {
        append_buffer_append(append_buffer, "~", 1);
      }
    } else {
      int length = E.row[file_row].render_size - E.col_offset;
      if (length < 0) {
        length = 0;
      }
      if (length > E.screen_cols) {
        length = E.screen_cols;
      }
      char *c = &E.row[file_row].render[E.col_offset];
      unsigned char *highlight = &E.row[file_row].highlight[E.col_offset];
      int current_color = -1;
      int j;
      for (j = 0; j < length; j++) {
        if (highlight[j] == HL_NORMAL) {
          if (current_color != -1) {
            append_buffer_append(append_buffer, "\x1b[39m", 5);
            current_color = -1;
          }
          append_buffer_append(append_buffer, &c[j], 1);
        } else {
          int color = editor_syntax_to_color(highlight[j]);
          if (color != current_color) {
            current_color = color;
            char buffer[16];
            int color_length = snprintf(
              buffer,
              sizeof(buffer),
              "\x1b[%dm", color);
            append_buffer_append(append_buffer, buffer, color_length);
          }
          append_buffer_append(append_buffer, &c[j], 1);
        }
      }
      append_buffer_append(append_buffer, "\x1b[39m", 5);
    }

    append_buffer_append(append_buffer, "\x1b[K", 3);
    append_buffer_append(append_buffer, "\r\n", 2);
  }
}

void editor_draw_status_bar(struct append_buffer* append_buffer) {
  append_buffer_append(append_buffer, "\x1b[7m", 4);
  char status[80];
  char right_status[80];
  int len = snprintf(
    status,
    sizeof(status),
    "%s.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]",
    E.num_rows,
    E.dirty ? "(modified)" : "");
  int right_len = snprintf(
    right_status,
    sizeof(right_status),
    "%d/%d",
    E.cursor_y + 1,
    E.num_rows);
  if (len > E.screen_cols) {
    len = E.screen_cols;
  }
  append_buffer_append(append_buffer, status, len);
  while (len < E.screen_cols) {
    if (E.screen_cols - len == right_len) {
      append_buffer_append(append_buffer, right_status, right_len);
      break;
    } else {
      append_buffer_append(append_buffer, " ", 1);
      len++;
    }
  }
  append_buffer_append(append_buffer, "\x1b[m", 3);
  append_buffer_append(append_buffer, "\r\n", 2);
}

void editor_draw_message_bar(struct append_buffer *append_buffer) {
  append_buffer_append(append_buffer, "\x1b[K", 3);
  int msg_len = strlen(E.status_msg);
  if (msg_len > E.screen_cols) {
    msg_len = E.screen_cols;
  }
  if (msg_len && time(NULL) - E.status_msg_time < 5) {
    append_buffer_append(append_buffer, E.status_msg, msg_len);
  }
}

void editor_refresh_screen(void) {
  editor_scroll();

  struct append_buffer append_buffer = APPEND_BUFFER_INIT;

  append_buffer_append(&append_buffer, "\x1b[?25l", 6);
  append_buffer_append(&append_buffer, "\x1b[H", 3);

  editor_draw_rows(&append_buffer);
  editor_draw_status_bar(&append_buffer);
  editor_draw_message_bar(&append_buffer);

  char buffer[32];
  snprintf(
    buffer,
    sizeof(buffer),
    "\x1b[%d;%dH",
    (E.cursor_y - E.row_offset) + 1,
    (E.render_x - E.col_offset) + 1);
  append_buffer_append(&append_buffer, buffer, strlen(buffer));

  append_buffer_append(&append_buffer, "\x1b[?25h", 6);

  write(STDOUT_FILENO, append_buffer.buffer, append_buffer.length);
  append_buffer_free(&append_buffer);
}

void editor_set_status_message(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
  va_end(ap);
  E.status_msg_time = time(NULL);
}

/*** input ***/

char *editor_prompt(char *prompt, void (*callback)(char *, int)) {
  size_t buffer_size = 128;
  char *buffer = malloc(buffer_size);

  size_t buffer_length = 0;
  buffer[0] = '\0';

  while (1) {
    editor_set_status_message(prompt, buffer);
    editor_refresh_screen();

    int c = editor_read_key();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buffer_length != 0) {
        buffer[--buffer_length] = '\0';
      }
    }
    if (c == '\x1b') {
      editor_set_status_message("");
      if (callback) {
        callback(buffer, c);
      }
      free(buffer);
      return NULL;
    } else if (c == '\r') {
      if (buffer_length != 0) {
        editor_set_status_message("");
        if (callback) {
          callback(buffer, c);
        }
        return buffer;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buffer_length == buffer_size - 1) {
        buffer_size *= 2;
        buffer = realloc(buffer, buffer_size);
      }
      buffer[buffer_length++] = c;
      buffer[buffer_length] = '\0';
    }

    if (callback) {
      callback(buffer, c);
    }
  }
}

void editor_move_cursor(int key) {
  editor_row *row = (E.cursor_y >= E.num_rows) ? NULL : &E.row[E.cursor_y];

  switch (key) {
    case ARROW_LEFT:
      if (E.cursor_x != 0) {
        E.cursor_x--;
      } else if (E.cursor_y > 0) {
        E.cursor_y--;
        E.cursor_x = E.row[E.cursor_y].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cursor_x < row->size) {
        E.cursor_x++;
      } else if (row && E.cursor_x == row->size) {
        E.cursor_y++;
        E.cursor_x = 0;
      }
      break;
    case ARROW_UP:
      if (E.cursor_y != 0) {
        E.cursor_y--;
      }
      break;
    case ARROW_DOWN:
      if (E.cursor_y != E.num_rows) {
        E.cursor_y++;
      }
      break;
  }

  row = (E.cursor_y >= E.num_rows) ? NULL : &E.row[E.cursor_y];
  int row_length = row ? row->size : 0;
  if (E.cursor_x > row_length) {
    E.cursor_x = row_length;
  }
}

void editor_process_keypress(void) {
  static int quit_times = KILO_QUIT_TIMES;

  int c = editor_read_key();

  switch (c) {
    case '\r':
      editor_insert_newline();
      break;

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editor_set_status_message(
          "WARNING!!! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.",
          quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editor_save();
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editor_move_cursor(c);
      break;
    case HOME_KEY:
      E.cursor_x = 0;
      break;
    case END_KEY:
      if (E.cursor_y < E.num_rows) {
        E.cursor_x = E.row[E.cursor_y].size;
      }
      break;

    case CTRL_KEY('f'):
      editor_find();
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) {
        editor_move_cursor(ARROW_RIGHT);
      }

      editor_del_char();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cursor_y = E.row_offset;
        } else if (c == PAGE_DOWN) {
          E.cursor_y = E.row_offset + E.screen_rows - 1;
          if (E.cursor_y > E.num_rows) {
            E.cursor_y = E.num_rows;
          }
        }

        int times = E.screen_rows;
        while (times--) {
          editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
      }
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      editor_insert_char(c);
      break;
  }

  quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void init_editor(void) {
  E.cursor_x = 0;
  E.cursor_y = 0;
  E.render_x = 0;
  E.row_offset = 0;
  E.col_offset = 0;
  E.num_rows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.status_msg[0] = '\0';
  E.status_msg_time = 0;

  if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
    die("get_window_size");
  }
  E.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
  enable_raw_mode();
  init_editor();

  if (argc >= 2) {
    editor_open(argv[1]);
  }

  editor_set_status_message(
    "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find"
  );

  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }
  return 0;
}
