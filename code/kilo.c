//
// includes
//

#include <assert.h> // assert
#include <ctype.h> // iscntrl
#include <errno.h> // errno, EAGAIN
#include <fcntl.h> // open, O_RDONLY
#include <limits.h> // UCHAR_MAX
#include <poll.h> // struct pollfd, nfds_t, poll, POLLIN
#include <stdarg.h>
#include <stdio.h> // perror, printf, snprintf
#include <stdlib.h> // atexit, exit, free, malloc, realloc, EXIT_SUCCESS
#include <string.h> // memcpy, strlen
#include <sys/ioctl.h> // struct winsize, ioctl, TIOCGWINSZ
#include <termios.h> // struct termios, tcgetattr, tcsetattr, BRKINT, CS8, ECHO, ICANON,
                     // ICRNL, IEXTEN, INPCK, ISIG, ISTRIP, IXON, OPOST, TCSAFLUSH, VMIN,
                     // VTIME
#include <time.h> // time_t
#include <unistd.h> // read, write, STDIN_FILENO, STDOUT_FILENO


//
// defines
//

#define KILO_VERSION "0.0.1"

#define KILO_TAB_STOP 8

#define ARRAYLEN(array) (sizeof(array)/sizeof((array)[0]))

#define CAST(type) (type)
#define CTRL_KEY(key) ((key) & 0x1f)


typedef enum EditorKey
{
    ARROW_LEFT = UCHAR_MAX + 1,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    END_KEY,
    HOME_KEY,
    PAGE_DOWN,
    PAGE_UP,
} EditorKey;


//
// data
//

typedef struct Buffer
{
    unsigned len;
    char *b;
} Buffer;


typedef struct Line
{
    Buffer raw;
    Buffer render;
} Line;


typedef struct Editor
{
    unsigned screenrows;
    unsigned screencols;
    unsigned cx, cy;
    unsigned rx;
    unsigned rowoff;
    unsigned coloff;
    unsigned numlines;
    Line *lines;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
} Editor;

static Editor editor;


//
// terminal
//

static _Noreturn void
die(const char *message)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(message);
    exit(EXIT_FAILURE);
}

static void
disableRawMode(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor.orig_termios);
}


static void
enableRawMode(void)
{
    if (-1 == tcgetattr(STDIN_FILENO, &editor.orig_termios))
    {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = editor.orig_termios;
    raw.c_cflag |= CS8;
    raw.c_iflag &= ~(CAST(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_lflag &= ~(CAST(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_oflag &= ~(CAST(tcflag_t)OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (-1 == tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw))
    {
        die("tcsetattr");
    }
}


static int
terminalHasKey(int timeout)
{
    nfds_t nfds = 1;
    struct pollfd fd = {
        .fd = STDIN_FILENO,
        .events = POLLIN,
    };
    int result = poll(&fd, nfds, timeout);
    return result;
}


static char
terminalNextKey(int timeout)
{
    char c = 0;
    if (terminalHasKey(timeout))
    {
        ssize_t nread = read(STDIN_FILENO, &c, 1);
        if ((-1 == nread) && (EAGAIN != errno))
        {
            die("read");
        }
    }
    return c;
}



static int
terminalReadKey(void)
{
    int c = terminalNextKey(-1);
    if ('\x1b' == c)
    {
        switch (terminalNextKey(0))
        {
            case 0:
            {
                // Just return the escape character
            } break;

            case '[':
            {
                char next = terminalNextKey(0);
                if ((next >= '0' && next <= '9'))
                {
                    if ('~' == terminalNextKey(0))
                    {
                        switch (next)
                        {
                            case '1':
                            {
                                c = HOME_KEY;
                            } break;

                            case '3':
                            {
                                c = DEL_KEY;
                            } break;

                            case '4':
                            {
                                c = END_KEY;
                            } break;

                            case '5':
                            {
                                c = PAGE_UP;
                            } break;

                            case '6':
                            {
                                c = PAGE_DOWN;
                            } break;

                            case '7':
                            {
                                c = HOME_KEY;
                            } break;

                            case '8':
                            {
                                c = END_KEY;
                            } break;

                            default:
                            {
                                // Unhandled escape code
                                c = 0;
                            } break;
                        }
                    }
                    else
                    {
                        // Unhandled escape code
                        c = 0;
                    }
                }
                else
                {
                    switch (next)
                    {
                        case 'A':
                        {
                            c = ARROW_UP;
                        } break;

                        case 'B':
                        {
                            c = ARROW_DOWN;
                        } break;

                        case 'C':
                        {
                            c = ARROW_RIGHT;
                        } break;

                        case 'D':
                        {
                            c = ARROW_LEFT;
                        } break;

                        case 'F':
                        {
                            c = END_KEY;
                        } break;

                        case 'H':
                        {
                            c = HOME_KEY;
                        } break;

                        default:
                        {
                            // Unhandled escape code
                            c = 0;
                        } break;
                    }
                }
            } break;

            case 'O':
            {
                switch (terminalNextKey(0))
                {
                    case 'F':
                    {
                        c = END_KEY;
                    } break;

                    case 'H':
                    {
                        c = HOME_KEY;
                    } break;

                    default:
                    {
                        // Unhandled escape code
                        c = 0;
                    } break;
                }
            } break;

            default:
            {
                // Unhandled escape code
                c = 0;
            } break;
        }
    }

    return c;
}


static int
getWindowSize(unsigned *rows, unsigned *cols)
{
    int result = 0;
    struct winsize ws;

    if (((-1 == ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)) && (0 != errno))
        || (0 == ws.ws_col))
    {
        result = -1;
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }

    return result;
}


//
// string buffer
//


static void
buffer_init(Buffer *buffer)
{
    buffer->len = 0;
    buffer->b = 0;
}


static void
buffer_append(Buffer *buffer, const char *s, unsigned len)
{
    if (len)
    {
        assert((buffer->len + len) > buffer->len);
        char *new = realloc(buffer->b, buffer->len + len);

        if (new)
        {
            memcpy(new + buffer->len, s, len);
            buffer->b = new;
            buffer->len += len;
        }
    }
}


static void
buffer_free(Buffer *buffer)
{
    free(buffer->b);
    buffer->b = 0;
    buffer->len = 0;
}


//
// row operations
//


static unsigned
editorRowCxToRx(Line *line, unsigned cx)
{
    unsigned rx = 0;
    for (unsigned i = 0; i < cx; ++i)
    {
        if ('\t' == line->raw.b[i])
        {
            rx += (KILO_TAB_STOP - 1) - (rx & KILO_TAB_STOP);
        }
        ++rx;
    }

    return rx;
}

static void
editorUpdateRow(Line *line)
{
    unsigned r = 0;
    for (unsigned i = 0; i < line->raw.len; ++i)
    {
        char c = line->raw.b[i];
        if ('\t' == c)
        {
            c = ' ';
            do
            {
                ++r;
                buffer_append(&line->render, &c, 1);
            } while (r % KILO_TAB_STOP);
        }
        else
        {
            ++r;
            buffer_append(&line->render, &c, 1);
        }
    }
}


static void
editorAppendRow(Buffer *new)
{
    Line *lines = realloc(editor.lines, sizeof(*lines) * (editor.numlines + 1));
    if (!lines)
    {
        die("editorAppendRow");
    }

    editor.lines = lines;
    Line *line = editor.lines + editor.numlines++;
    line->raw = *new;
    line->render.len = 0;
    line->render.b = 0;
    editorUpdateRow(line);

    new->len = 0;
    new->b = 0;
}


//
// file i/o
//


static void
editorOpen(const char *filename)
{
    free(editor.filename);

    int fd = open(filename, O_RDONLY);
    if (-1 == fd)
    {
        die("open");
    }

    editor.filename = malloc(strlen(filename) + 1);
    if (editor.filename)
    {
        size_t i;
        for (i = 0; i < strlen(filename); ++i)
        {
            editor.filename[i] = filename[i];
        }
        editor.filename[i] = 0;
    }
    else
    {
        die("strdup");
    }

    ssize_t nread;
    do
    {
        Buffer line = {0};
        char c;
        do
        {
            nread = read(fd, &c, 1);
            if (nread < 0)
            {
                die("reading file");
            }
            else if (nread)
            {
                buffer_append(&line, &c, 1);
            }
            else
            {
                break;
            }
        } while ('\n' != c);

        if (line.len)
        {
            --line.len;
            if (line.len && ('\r' == line.b[line.len - 1]))
            {
                --line.len;
            }
            editorAppendRow(&line);
        }
    } while (nread);
}


//
// input
//

static void
editorMoveCursor(int key)
{
    Line *line = 0;
    if (editor.cy < editor.numlines)
    {
        line = editor.lines + editor.cy;
    }

    switch (key)
    {
        case ARROW_LEFT:
        {
            if (editor.cx)
            {
                --editor.cx;
            }
            else if (editor.cy)
            {
                --editor.cy;
                editor.cx = editor.lines[editor.cy].raw.len;
            }
        } break;

        case ARROW_RIGHT:
        {
            if (line)
            {
                if (editor.cx < line->raw.len)
                {
                    ++editor.cx;
                }
                else if (editor.cy < editor.numlines)
                {
                    ++editor.cy;
                    editor.cx = 0;
                }
            }
        } break;

        case ARROW_DOWN:
        {
            if (editor.cy < editor.numlines)
            {
                ++editor.cy;
            }
        } break;

        case ARROW_UP:
        {
            if (editor.cy)
            {
                --editor.cy;
            }
        } break;
    }

    if (editor.cy < editor.numlines)
    {
        line = editor.lines + editor.cy;
        if (editor.cx > line->raw.len)
        {
            editor.cx = line->raw.len;
        }
    }
    else
    {
        editor.cx = 0;
    }
}


static void
editorProcessKeyPress(void)
{
    int c = terminalReadKey();

    switch (c)
    {
        case CTRL_KEY('q'):
        {
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(EXIT_SUCCESS);
        } break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
        {
            editorMoveCursor(c);
        } break;

        case END_KEY:
        {
            if (editor.cy < editor.numlines)
            {
                editor.cx = editor.lines[editor.cy].raw.len;
            }
        } break;

        case HOME_KEY:
        {
            editor.cx = 0;
        } break;

        case PAGE_DOWN:
        {
            for (unsigned n = 1; n < editor.screenrows; ++n)
            {
                editorMoveCursor(ARROW_DOWN);
            }
        } break;

        case PAGE_UP:
        {
            for (unsigned n = 1; n < editor.screenrows; ++n)
            {
                editorMoveCursor(ARROW_UP);
            }
        } break;
    }
}


//
// output
//


void editorScroll(void)
{
    editor.rx = editor.cx;
    if (editor.cy < editor.numlines)
    {
        editor.rx = editorRowCxToRx(editor.lines + editor.cy, editor.cx);
    }

    if (editor.cy < editor.rowoff)
    {
        editor.rowoff = editor.cy;
    }
    else if (editor.cy >= (editor.rowoff + editor.screenrows))
    {
        editor.rowoff = editor.cy - editor.screenrows + 1;
    }

    if (editor.rx < editor.coloff)
    {
        editor.coloff = editor.rx;
    }
    else if (editor.rx >= (editor.coloff + editor.screencols))
    {
        editor.coloff = editor.rx - editor.screencols + 1;
    }
}


static void
editorDrawRows(Buffer *buffer)
{
    for (unsigned y = 0; y < editor.screenrows; ++y)
    {
        unsigned row = y + editor.rowoff;
        if (row >= editor.numlines)
        {
            if (!editor.numlines && (y == (editor.screenrows / 3)))
            {
                char welcome[80];
                int result = snprintf(
                        welcome, sizeof(welcome), "Kilo editor -- version %s",
                        KILO_VERSION);
                if (result < 0)
                {
                    die("snprintf");
                }
                unsigned welcomelen = CAST(unsigned)result;
                if (welcomelen > editor.screencols)
                {
                    welcomelen = editor.screencols;
                }

                unsigned padding = (editor.screencols - welcomelen) / 2;
                if (padding)
                {
                    buffer_append(buffer, "~", 1);
                    while (--padding)
                    {
                        buffer_append(buffer, " ", 1);
                    }
                }

                buffer_append(buffer, welcome, CAST(unsigned)welcomelen);
            }
            else
            {
                buffer_append(buffer, "~", 1);
            }
        }
        else
        {
            Line *line = editor.lines + row;
            unsigned len = line->render.len;
            if (len < editor.coloff)
            {
                len = 0;
            }
            else
            {
                len -= editor.coloff;
                if (len > editor.screencols)
                {
                    len = editor.screencols;
                }
            }

            buffer_append(buffer, line->render.b + editor.coloff, len);
        }

        buffer_append(buffer, "\x1b[K", 3);
        buffer_append(buffer, "\r\n", 2);
    }
}


static void
editorDrawStatusBar(Buffer *buffer)
{
    buffer_append(buffer, "\x1b[7m", 4);
    char status[80];
    int result = snprintf(status, sizeof(status), "%.20s - %u lines",
        editor.filename ? editor.filename : "[No Name]", editor.numlines);
    if (result < 0)
    {
        die("status bar");
    }

    unsigned len = CAST(unsigned)result;
    if (len > editor.screencols)
    {
        len = editor.screencols;
    }
    buffer_append(buffer, status, len);

    result = snprintf(status, sizeof(status), "%u/%u", editor.cy + 1, editor.numlines);
    if (result < 0)
    {
        die("status bar");
    }
    unsigned pos = len;
    len = CAST(unsigned)result;

    while (pos < editor.screencols)
    {
        if ((pos + len) == editor.screencols)
        {
            buffer_append(buffer, status, len);
            break;
        }
        else
        {
            ++pos;
            buffer_append(buffer, " ", 1);
        }
    }

    buffer_append(buffer, "\x1b[m\r\n", 5);
}


static void
editorDrawMessageBar(Buffer *buffer)
{
    buffer_append(buffer, "\x1b[K", 3);
    unsigned msglen = CAST(unsigned)strlen(editor.statusmsg);
    if (msglen > editor.screencols)
    {
        msglen = editor.screencols;
    }
    if (msglen && ((time(0) - editor.statusmsg_time) < 5))
    {
        buffer_append(buffer, editor.statusmsg, msglen);
    }
}


static void
editorRefreshScreen(void)
{
    editorScroll();

    Buffer buffer;
    buffer_init(&buffer);

    buffer_append(&buffer, "\x1b[?25l", 6);
    buffer_append(&buffer, "\x1b[H", 3);

    editorDrawRows(&buffer);
    editorDrawStatusBar(&buffer);
    editorDrawMessageBar(&buffer);

    char buf[32];
    snprintf(
        buf, sizeof(buf), "\x1b[%d;%dH",
        editor.cy - editor.rowoff + 1,
        editor.rx - editor.coloff + 1);
    buffer_append(&buffer, buf, CAST(unsigned)strlen(buf));

    buffer_append(&buffer, "\x1b[?25h", 6);

    write(STDOUT_FILENO, buffer.b, buffer.len);
    buffer_free(&buffer);
}


static void
editorSetStatusMessage(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(editor.statusmsg, sizeof(editor.statusmsg), fmt, args);
    va_end(args);
    editor.statusmsg_time = time(0);
}


//
// init
//

static void
initEditor(void)
{
    editor.cx = 0;
    editor.cy = 0;
    editor.rx = 0;
    editor.rowoff = 0;
    editor.coloff = 0;
    editor.numlines = 0;
    editor.lines = 0;
    editor.filename = 0;
    editor.statusmsg[0] = 0;
    editor.statusmsg_time = 0;
    if (getWindowSize(&editor.screenrows, &editor.screencols))
    {
        die("getWindowSize");
    }
    editor.screenrows -= 2;
}


int main(int argc, char **argv)
{
    initEditor();
    if (argc > 1)
    {
        editorOpen(argv[1]);
    }
    editorSetStatusMessage("Help: Ctrl-Q = quit");

    enableRawMode();
    for (;;)
    {
        editorRefreshScreen();
        editorProcessKeyPress();
    }

    return EXIT_SUCCESS;
}
