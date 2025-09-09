/*** includes ***/

#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

/*** defines ***/

#define CTRL_KEY(k) ((k) & 0x1f)

/*** data ***/

struct editor_config {
  int screen_rows;
  int screen_cols;
  struct termios orig_termios;
};

struct editor_config E;

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

char editor_read_key(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
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

void editor_process_keypress(void) {
  char c = editor_read_key();

  switch (c) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
  }
}

void editor_draw_rows(struct append_buffer *append_buffer) {
  int y;
  for (y = 0; y < E.screen_rows; y++) {
    append_buffer_append(append_buffer, "~", 1);

    append_buffer_append(append_buffer, "\x1b[K", 3);
    if (y < E.screen_rows - 1) {
      append_buffer_append(append_buffer, "\r\n", 2);
    }
  }
}

void editor_refresh_screen(void) {
  struct append_buffer append_buffer = APPEND_BUFFER_INIT;

  append_buffer_append(&append_buffer, "\x1b[?25l", 6);
  append_buffer_append(&append_buffer, "\x1b[2J", 4);
  append_buffer_append(&append_buffer, "\x1b[H", 3);

  editor_draw_rows(&append_buffer);

  append_buffer_append(&append_buffer, "\x1b[H", 3);
  append_buffer_append(&append_buffer, "\x1b[?25h", 6);

  write(STDOUT_FILENO, append_buffer.buffer, append_buffer.length);
  append_buffer_free(&append_buffer);
}

/*** init ***/

void init_editor(void) {
  if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) {
    die("get_window_size");
  }
}

int main(void) {
  enable_raw_mode();
  init_editor();

  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }
  return 0;
}
