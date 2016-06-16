#define _GNU_SOURCE
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <sys/ioctl.h>
#include "window.h"
#include "message.h"
#include "clog.h"

struct winmgr
{
    window *root;
    int invalid;
    handler h;
    int resize[2];
};

struct window
{
    winmgr *wm;
    window *next;
    window *parent;
    window *child;
    WINDOW *win;
    int invalid;
    int visible;
    int id;
    rect rc;
    handler h;
};

static int s_sigwinch_installed;
static struct sigaction s_sigwinch_sigaction_old;
static int s_resize_pipe[2];
static int s_pipe_signaled;
static void sigwinch_signal_handler(int sig);

window *new_window(winmgr *wm, window *parent, WINDOW *win, const rect *rc, handler h, int id);
uint32_t winmgr_proc(winmgr *wm, int id, const message_data *data);
void window_invalidate_rect(window *w, const rect *rc);
static winmgr *s_winmgr;

window *winmgr_init()
{
    winmgr *wm = s_winmgr;
    if (wm)
        return wm->root;

    // Initialize messaging
    message_init(sizeof(message_data));

    // Init ncurses mode
    initscr();

    // Don't buffer keystrokes
    cbreak();

    // Suppress automatic echoing of typed characters
    noecho();

    // Enable backspace, delete, and four arrow keys
    keypad(stdscr, TRUE);

    // Turn off the cursor
    curs_set(0);

    // create the window manager object
    wm = malloc(sizeof(winmgr));
    memset(wm, 0, sizeof(*wm));
    wm->h = handler_create(wm, (handler_proc)winmgr_proc);
    s_winmgr = wm;

    // Create the root window
    rect rc;
    int cy, cx;
    getmaxyx(stdscr, cy, cx);
    rect_set(&rc, 0, 0, cx, cy);
    wm->root = new_window(wm, NULL, stdscr, &rc, 0, 0);

    // This allows the window manager to paint windows at
    // message queue idle time.
    message_set_hook(wm->h);

    // Resize handler
    if (!s_sigwinch_installed)
    {
        s_pipe_signaled = 0;
        pipe(s_resize_pipe);
        struct sigaction action;
        action.sa_handler = sigwinch_signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        if (sigaction(SIGWINCH, &action, &s_sigwinch_sigaction_old) < 0) {
            winmgr_shutdown();
            return NULL;
        }
        s_sigwinch_installed = 1;
    }

    return wm->root;
}

void winmgr_shutdown()
{
    winmgr *wm = s_winmgr;
    if (!wm)
        return;

    if (wm->root)
    {
        window_destroy(wm->root);
        wm->root = NULL;
    }
    handler_destroy(wm->h);
    free(wm);

    endwin();

    s_winmgr = NULL;

    message_shutdown();

    if (s_sigwinch_installed == 1)
    {
        sigaction(SIGWINCH, &s_sigwinch_sigaction_old, NULL);
        close(s_resize_pipe[0]);
        close(s_resize_pipe[1]);
        s_sigwinch_installed = 0;
    }
}

void sigwinch_signal_handler(int sig)
{
    // Do nothing but signal this pipe, since the signal can occur between
    // any two instructions
    if (s_sigwinch_installed)
    {
        if (!s_pipe_signaled)
        {
            // Write to the pipe to wake up the select loop
            unsigned char b = 0;
            write(s_resize_pipe[1], &b, sizeof(b));
            s_pipe_signaled = 1;
        }

        if (s_sigwinch_sigaction_old.sa_handler)
            (s_sigwinch_sigaction_old.sa_handler)(sig);
    }
}

void winmgr_resize()
{
    // Reset the pipe if signaled
    if (s_sigwinch_installed && s_pipe_signaled)
    {
        unsigned char b;
        read(s_resize_pipe[0], &b, sizeof(b));
        s_pipe_signaled = 0;
    }

    winmgr *wm = s_winmgr;

    // Get the terminal size. Nothing to do if it is the size of the root
    // window already.
    struct winsize size;
    if (ioctl(fileno(stdout), TIOCGWINSZ, &size) == -1)
        return;
    int width = wm->root->rc.right - wm->root->rc.left;
    int height = wm->root->rc.bottom - wm->root->rc.top;
    if (width == size.ws_col && height == size.ws_row)
        return;

    // Set the terminal size
    resizeterm(size.ws_row, size.ws_col);

    // Set the root window rect. Children have the chance to resize
    // themselves.
    rect rc;
    rect_set(&rc, 0, 0, size.ws_col, size.ws_row);
    window_set_pos(wm->root, &rc);

    // Force an update now so that updating occurs while sizing.
    winmgr_update();
}

int winmgr_resize_fd()
{
    return s_resize_pipe[0];
}

window *find_invalid(window *w)
{
    if (!w->visible)
        return NULL;

    // If there are children, the children cover the parent
    // and the parent isn't invalid.
    if (w->child)
    {
        for (window *child = w->child; child != NULL; child = child->next)
        {
            window *wT = find_invalid(child);
            if (wT != NULL)
                return wT;
        }
        return NULL;
    }

    if (w->invalid)
        return w;

    return NULL;
}

void winmgr_update()
{
    // Typically this is called at idle time however sometimes an app may
    // want to force an update.
    winmgr *wm = s_winmgr;

    while (wm->invalid)
    {
        // Is there a window to paint?
        window *w = find_invalid(wm->root);
        if (w)
        {
            // Ask the window to paint.
            w->invalid = 0;
            handler_call(w->h, WM_PAINT, NULL);

            // Copy the ncurses WINDOW to the virtual screen
            wnoutrefresh(w->win);
        }
        else
        {
            // All done with painting. Update from the virtual screen
            // to the physical screen.
            doupdate();
            wm->invalid = 0;
        }
    }
}

uint32_t winmgr_proc(winmgr *wm, int id, const message_data *data)
{
    switch (id)
    {
    case MM_READABLE:
        winmgr_update();
        break;
    }

    return 0;
}

window *window_create(window *parent, const rect *rc, handler h, int id)
{
    winmgr *wm = s_winmgr;
    if (parent == NULL)
        parent = wm->root;

    // Coords passed in are parent relative. Make them screen relative.
    rect rcT = *rc;
    rect_offset(&rcT, parent->rc.left, parent->rc.top);

    // Clip to the screen to work around ncurses behavior
    rect_intersect(&rcT, &rcT, &wm->root->rc);

    // A window can be visible or hidden, or a window can be visible but without
    // ncurses WINDOW (like a container of other windows).
    WINDOW *win = newwin(rc->bottom - rc->top, rc->right - rc->left, rc->top, rc->left);
    if (win == NULL)
        return NULL;
    return new_window(parent->wm, parent, win, &rcT, h, id);
}

window *new_window(winmgr *wm, window *parent, WINDOW *win, const rect *rc, handler h, int id)
{
    window *w = malloc(sizeof(window));
    memset(w, 0, sizeof(*w));
    w->wm = wm;
    w->h = h;
    w->invalid = 0;
    w->win = win;
    w->rc = *rc;
    w->visible = 1;
    w->id = id;

    // Add it to the end of its siblings
    if (parent != NULL)
    {
        window **ppw = &parent->child;
        while (*ppw)
            ppw = &((*ppw)->next);
        *ppw = w;
        w->parent = parent;
    }

    // Notify client
    message_data data;
    memset(&data, 0, sizeof(data));
    data.create.w = w;
    handler_call(h, WM_CREATE, &data);

    // Mark it invalid so it gets a paint message
    window_invalidate(w);

    return w;
}

void window_destroy(window *w)
{
    // Destroy its children first
    for (window *wT = w->child; wT != NULL; wT = w->child)
    {
        window_destroy(wT);
    }

    handler_call(w->h, WM_DESTROY, NULL);

    // Unlink it    
    if (w->parent != NULL)
    {
        window **ppw = &w->parent->child;
        while (*ppw)
        {
            if (*ppw == w)
            {
                *ppw = w->next;
                break;
            }
            ppw = &((*ppw)->next);
        }
    }

    // Delete the window if it exists
    if (w->win != stdscr)
        delwin(w->win);

    // NOTE: nothing to invalidate?

    free(w);
}

handler window_set_handler(window *w, handler h)
{
    winmgr *wm = s_winmgr;
    if (w == NULL)
        w = wm->root;

    handler old = w->h;
    w->h = h;
    return old;
}

void window_set_visible(window *w, int visible)
{
    if (!visible)
    {
        if (w->visible)
        {
            w->visible = 0;
            if (w->parent != NULL)
                window_invalidate_rect(w->parent, &w->parent->rc);
        }
    }
    else
    {
        if (!w->visible)
        {
            w->visible = 1;
            window_invalidate(w);
        }
    }
}

void window_invalidate_rect(window *w, const rect *rc)
{
    // Mark invalid any visible leaf windows that intersect rc
    if (!w->visible)
        return;

    rect rcT;
    if (!rect_intersect(&rcT, &w->rc, rc))
        return;

    if (w->child)
    {
        for (window *child = w->child; child != NULL; child = child->next)
        {
            window_invalidate_rect(child, &rcT);
        }
        return;
    }

    w->invalid = 1;
    w->wm->invalid = 1;
    message_hook_readable();
}

void window_invalidate(window *w)
{
    winmgr *wm = s_winmgr;
    if (w == NULL)
        w = wm->root;

    if (!w->visible)
        return;

    // Clip to parents then invalidate down
    rect rcT = w->rc;
    for (window *parent = w->parent; parent != NULL; parent = parent->parent)
    {
        if (!parent->visible)
            return;
        if (!rect_intersect(&rcT, &rcT, &parent->rc))
            return;
    }

    window_invalidate_rect(w, &rcT);
}

WINDOW *window_WIN(window *w)
{
    winmgr *wm = s_winmgr;
    if (w == NULL)
        w = wm->root;

    return w->win;
}

int window_set_pos(window *w, const rect *rc)
{
    winmgr *wm = s_winmgr;

    // Convert to screen coords
    rect rc_new = *rc;
    if (w->parent)
        rect_offset(&rc_new, w->parent->rc.left, w->parent->rc.top);
    if (rect_equal(&w->rc, &rc_new))
        return 1;

    // Make sure it fits on-screen. ncurses behaves badly
    // if it is off screen in any way.
    if (w != wm->root)
    {
        rect_intersect(&rc_new, &rc_new, &wm->root->rc);
    }

    int height_new = rc_new.bottom - rc_new.top;
    int width_new = rc_new.right - rc_new.left;

    if (rc_new.left != w->rc.left || rc_new.top != w->rc.top)
    {
        // Pre-size to a size that won't cause mvwin to fail.
        int width_adj = (w->rc.left + width_new) - wm->root->rc.right;
        if (width_adj < 0)
            width_adj = 0;
        int height_adj = (w->rc.top + height_new) - wm->root->rc.bottom;
        if (height_adj < 0)
            height_adj = 0;
        if (width_adj != 0 || height_adj != 0)
        {
            if (wresize(w->win, height_new - height_adj, width_new - width_adj) == ERR)
                return 0;
        }
        if (mvwin(w->win, rc_new.top, rc_new.left) == ERR)
            return 0;
    }
    if (wresize(w->win, height_new, width_new) == ERR)
        return 0;
    rect rc_old = w->rc;
    w->rc = rc_new;

    // Invalidate affected windows
    rect rc_invalid;
    rect_union(&rc_invalid, &rc_old, &rc_new);
    if (w->parent)
    {
        window_invalidate_rect(w->parent, &rc_invalid);
    }
    else
    {
        // w is the root window.
        window_invalidate_rect(w, &rc_invalid);
    }

    // Notify 
    if (w->parent)
    {
        rect_offset(&rc_old, -w->parent->rc.left, -w->parent->rc.top);
        rect_offset(&rc_new, -w->parent->rc.left, -w->parent->rc.top);
    }
    message_data data;
    memset(&data, 0, sizeof(data));
    data.pos_changed.rc_old = &rc_old;
    data.pos_changed.rc_new = &rc_new;
    handler_call(w->h, WM_POSCHANGED, &data);
    return 1;
}

window *window_find_window(window *w, int id)
{
    winmgr *wm = s_winmgr;
    if (w == NULL)
        w = wm->root;

    for (window *child = w->child; child != NULL; child = child->next)
    {
        if (child->id == id)
            return child;
    }

    return NULL;
}

void window_rect(window *w, rect *rc)
{
    winmgr *wm = s_winmgr;
    if (w == NULL)
        w = wm->root;

    *rc = w->rc;
    if (w->parent != NULL)
        rect_offset(rc, -w->parent->rc.left, -w->parent->rc.top);
}
