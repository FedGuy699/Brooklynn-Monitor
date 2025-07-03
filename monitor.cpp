#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <pwd.h>
#include <signal.h>
#include <chrono>

Display* display;
Window root;
int selected_index = -1;
int scroll_offset = 0;

const int WINDOW_WIDTH = 800;
const int WINDOW_HEIGHT = 600;

const int BUTTON_WIDTH = 140;
const int BUTTON_HEIGHT = 40;
const int BUTTON_MARGIN = 15;

const int SCROLLBAR_WIDTH = 15;
const int ITEM_HEIGHT = 20;
const int SEARCH_HEIGHT = 40;
const int VISIBLE_ITEMS = (WINDOW_HEIGHT - SEARCH_HEIGHT - BUTTON_HEIGHT - BUTTON_MARGIN) / ITEM_HEIGHT;

bool mouse_over_button = false;
bool mouse_over_scrollbar = false;
bool scrollbar_dragging = false;
int scrollbar_drag_start = 0;
int scrollbar_drag_offset = 0;

std::string search_text;
bool search_focused = true;
bool cursor_visible = true;
auto last_cursor_blink = std::chrono::steady_clock::now();

std::string get_username(uid_t uid) {
    struct passwd *pw = getpwuid(uid);
    return pw ? pw->pw_name : "unknown";
}

std::vector<std::string> get_detailed_processes() {
    std::vector<std::string> processes;

    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return processes;

    struct dirent* entry;
    while ((entry = readdir(proc_dir)) != nullptr) {
        if (!std::all_of(entry->d_name, entry->d_name + strlen(entry->d_name), ::isdigit))
            continue;

        std::string pid(entry->d_name);
        std::string base = "/proc/" + pid;

        std::ifstream status_file(base + "/status");
        std::ifstream cmd_file(base + "/cmdline");

        if (!status_file.is_open() || !cmd_file.is_open()) continue;

        std::string line;
        uid_t uid = 0;
        while (std::getline(status_file, line)) {
            if (line.find("Uid:") == 0) {
                std::istringstream iss(line);
                std::string label;
                iss >> label >> uid;
                break;
            }
        }

        if (uid != getuid()) continue;

        std::string cmdline;
        std::getline(cmd_file, cmdline, '\0');

        if (cmdline.empty()) {
            char exe_path[1024] = {0};
            std::string exe_link = base + "/exe";
            ssize_t len = readlink(exe_link.c_str(), exe_path, sizeof(exe_path) - 1);
            if (len > 0) {
                cmdline = std::string(exe_path, len);
            } else {
                std::ifstream comm_file(base + "/comm");
                if (comm_file.is_open()) {
                    std::getline(comm_file, cmdline);
                }
                if (cmdline.empty()) {
                    cmdline = "[kernel or zombie process]";
                }
            }
        }

        std::string user = get_username(uid);
        std::ostringstream oss;
        oss.width(6); oss << std::left << pid << "  ";
        oss.width(10); oss << std::left << user << "  ";
        oss << cmdline;
        processes.push_back(oss.str());
    }

    closedir(proc_dir);
    return processes;
}

void draw_rounded_rect(Display* dpy, Drawable d, GC gc, int x, int y, int w, int h, int radius) {
    XFillRectangle(dpy, d, gc, x + radius, y, w - 2*radius, h);
    XFillRectangle(dpy, d, gc, x, y + radius, w, h - 2*radius);
    XFillArc(dpy, d, gc, x, y, 2*radius, 2*radius, 90 * 64, 90 * 64);
    XFillArc(dpy, d, gc, x + w - 2*radius, y, 2*radius, 2*radius, 0 * 64, 90 * 64);
    XFillArc(dpy, d, gc, x, y + h - 2*radius, 2*radius, 2*radius, 180 * 64, 90 * 64);
    XFillArc(dpy, d, gc, x + w - 2*radius, y + h - 2*radius, 2*radius, 2*radius, 270 * 64, 90 * 64);
}

void draw_button(Display* display, Window win, GC gc, const char* label) {
    int x = WINDOW_WIDTH - BUTTON_WIDTH - BUTTON_MARGIN - SCROLLBAR_WIDTH;
    int y = WINDOW_HEIGHT - BUTTON_HEIGHT - BUTTON_MARGIN;
    
    XSetForeground(display, gc, 0x444444); 
    XFillRectangle(display, win, gc, x + 3, y + 3, BUTTON_WIDTH, BUTTON_HEIGHT);

    if (mouse_over_button)
        XSetForeground(display, gc, 0x3377CC);
    else
        XSetForeground(display, gc, 0x5599FF);
    
    draw_rounded_rect(display, win, gc, x, y, BUTTON_WIDTH, BUTTON_HEIGHT, 8);

    XSetForeground(display, gc, 0x113366);
    XDrawRectangle(display, win, gc, x, y, BUTTON_WIDTH, BUTTON_HEIGHT);

    XSetForeground(display, gc, 0xFFFFFF);
    int len = strlen(label);
    XFontStruct *font = XQueryFont(display, XGContextFromGC(gc));
    int font_height = font ? font->ascent + font->descent : 12;

    int text_width = len * 8; 
    int text_x = x + (BUTTON_WIDTH - text_width) / 2;
    int text_y = y + (BUTTON_HEIGHT + font_height) / 2 - 3;
    XDrawString(display, win, gc, text_x, text_y, label, len);
}

void draw_search_bar(Display* display, Window win, GC gc) {
    int x = 20;
    int y = 10;
    int w = WINDOW_WIDTH - 40 - SCROLLBAR_WIDTH;
    int h = 30;

    if (search_focused)
        XSetForeground(display, gc, 0xFFFFFF); 
    else
        XSetForeground(display, gc, 0xF0F0F0); 
    draw_rounded_rect(display, win, gc, x, y, w, h, 8);

    XSetForeground(display, gc, 0x888888);
    XDrawRectangle(display, win, gc, x, y, w, h);

    XSetForeground(display, gc, 0x000000);
    std::string display_text = search_text.empty() ? "Search..." : search_text;
    if (search_text.empty())
        XSetForeground(display, gc, 0xAAAAAA);

    int text_x = x + 10;
    int text_y = y + h/2 + 6;
    XDrawString(display, win, gc, text_x, text_y, display_text.c_str(), display_text.length());

    if (search_focused && cursor_visible) {
        int cursor_x = text_x + 8 * search_text.length();
        int cursor_y = y + 6;
        int cursor_h = h - 12;
        XDrawLine(display, win, gc, cursor_x, cursor_y, cursor_x, cursor_y + cursor_h);
    }
}

void draw_scrollbar(Display* display, Window win, GC gc, int total_items) {
    if (total_items <= VISIBLE_ITEMS) return;

    int scrollbar_x = WINDOW_WIDTH - SCROLLBAR_WIDTH;
    int scrollbar_height = WINDOW_HEIGHT - SEARCH_HEIGHT - BUTTON_HEIGHT - BUTTON_MARGIN;

    XSetForeground(display, gc, 0xE0E0E0);
    XFillRectangle(display, win, gc, scrollbar_x, SEARCH_HEIGHT, SCROLLBAR_WIDTH, scrollbar_height);

    double items_ratio = (double)VISIBLE_ITEMS / total_items;
    int thumb_height = std::max(30, (int)(scrollbar_height * items_ratio));
    double scroll_ratio = (double)scroll_offset / (total_items - VISIBLE_ITEMS);
    int thumb_y = SEARCH_HEIGHT + (int)((scrollbar_height - thumb_height) * scroll_ratio);

    if (mouse_over_scrollbar || scrollbar_dragging)
        XSetForeground(display, gc, 0x888888);
    else
        XSetForeground(display, gc, 0xAAAAAA);
    XFillRectangle(display, win, gc, scrollbar_x, thumb_y, SCROLLBAR_WIDTH, thumb_height);

    XSetForeground(display, gc, 0x888888);
    XDrawRectangle(display, win, gc, scrollbar_x, SEARCH_HEIGHT, SCROLLBAR_WIDTH, scrollbar_height);
}

void draw_process_list(Display* display, Window win, GC gc, const std::vector<std::string>& processes) {
    XSetForeground(display, gc, 0xFFFFFF);
    XFillRectangle(display, win, gc, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);

    draw_search_bar(display, win, gc);

    int y = SEARCH_HEIGHT;
    int start_index = scroll_offset;
    int end_index = std::min(start_index + VISIBLE_ITEMS, (int)processes.size());

    for (int i = start_index; i < end_index; ++i) {
        int item_y = y + (i - start_index) * ITEM_HEIGHT;

        if (i == selected_index) {
            XSetForeground(display, gc, 0xADD8E6);
            XFillRectangle(display, win, gc, 0, item_y, WINDOW_WIDTH - SCROLLBAR_WIDTH, ITEM_HEIGHT);
            XSetForeground(display, gc, 0x000000);
        } else if (i % 2 == 0) {
            XSetForeground(display, gc, 0xF0F8FF);
            XFillRectangle(display, win, gc, 0, item_y, WINDOW_WIDTH - SCROLLBAR_WIDTH, ITEM_HEIGHT);
            XSetForeground(display, gc, 0x000000);
        } else {
            XSetForeground(display, gc, 0xFFFFFF);
            XFillRectangle(display, win, gc, 0, item_y, WINDOW_WIDTH - SCROLLBAR_WIDTH, ITEM_HEIGHT);
            XSetForeground(display, gc, 0x000000);
        }

        XDrawString(display, win, gc, 10, item_y + ITEM_HEIGHT - 5, processes[i].c_str(), processes[i].length());
    }

    if (selected_index != -1) {
        draw_button(display, win, gc, "Kill Process");
    }

    draw_scrollbar(display, win, gc, processes.size());
}

pid_t get_pid_from_process_line(const std::string& line) {
    std::istringstream iss(line);
    std::string pid_str;
    iss >> pid_str;
    return (pid_t)std::stoi(pid_str);
}

bool contains_ignore_case(const std::string& haystack, const std::string& needle) {
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) {
            return std::toupper(ch1) == std::toupper(ch2);
        });
    return (it != haystack.end());
}

void update_scroll_position(int new_offset, int max_offset) {
    int old_offset = scroll_offset;
    scroll_offset = std::max(0, std::min(new_offset, max_offset));
    if (scroll_offset != old_offset) {
        XClearWindow(display, root);
    }
}

int main() {
    display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "Cannot open display.\n";
        return 1;
    }

    root = DefaultRootWindow(display);
    int screen = DefaultScreen(display);
    Window win = XCreateSimpleWindow(display, root, 100, 100, WINDOW_WIDTH, WINDOW_HEIGHT, 1,
                                     BlackPixel(display, screen), WhitePixel(display, screen));
    XStoreName(display, win, "Brooklynn Process Manager");
    XSelectInput(display, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | 
                 PointerMotionMask | FocusChangeMask);
    XMapWindow(display, win);

    Atom wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, win, &wm_delete_window, 1);

    GC gc = XCreateGC(display, win, 0, nullptr);
    XSetForeground(display, gc, BlackPixel(display, screen));

    Font font = XLoadFont(display, "9x15");
    XSetFont(display, gc, font);

    std::vector<std::string> all_processes = get_detailed_processes();
    std::vector<std::string> filtered_processes = all_processes;

    XEvent e;
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_cursor_blink).count() > 500) {
            cursor_visible = !cursor_visible;
            last_cursor_blink = now;
            draw_process_list(display, win, gc, filtered_processes);
        }

        while (XPending(display)) {
            XNextEvent(display, &e);

            if (e.type == Expose) {
                all_processes = get_detailed_processes();
                filtered_processes.clear();
                for (const auto& p : all_processes) {
                    if (search_text.empty() || contains_ignore_case(p, search_text))
                        filtered_processes.push_back(p);
                }
                if ((size_t)selected_index >= filtered_processes.size()) selected_index = -1;
                draw_process_list(display, win, gc, filtered_processes);
            }

            if (e.type == ClientMessage) {
                if ((Atom)e.xclient.data.l[0] == wm_delete_window) {
                    XCloseDisplay(display);
                    return 0;
                }
            }


            if (e.type == MotionNotify) {
                int x = e.xmotion.x;
                int y = e.xmotion.y;

                int btn_x = WINDOW_WIDTH - BUTTON_WIDTH - BUTTON_MARGIN - SCROLLBAR_WIDTH;
                int btn_y = WINDOW_HEIGHT - BUTTON_HEIGHT - BUTTON_MARGIN;
                bool was_over_button = mouse_over_button;
                mouse_over_button = (selected_index != -1 &&
                                    x >= btn_x && x <= btn_x + BUTTON_WIDTH &&
                                    y >= btn_y && y <= btn_y + BUTTON_HEIGHT);

                bool was_over_scrollbar = mouse_over_scrollbar;
                mouse_over_scrollbar = (x >= WINDOW_WIDTH - SCROLLBAR_WIDTH && 
                                       y >= SEARCH_HEIGHT && 
                                       y <= WINDOW_HEIGHT - BUTTON_HEIGHT - BUTTON_MARGIN);

                if (mouse_over_button != was_over_button || mouse_over_scrollbar != was_over_scrollbar) {
                    draw_process_list(display, win, gc, filtered_processes);
                }
                if (scrollbar_dragging) {
                    int scrollbar_height = WINDOW_HEIGHT - SEARCH_HEIGHT - BUTTON_HEIGHT - BUTTON_MARGIN;
                    double scroll_ratio = (double)(y - SEARCH_HEIGHT - scrollbar_drag_offset) / 
                                         (scrollbar_height - scrollbar_drag_offset);
                    int max_offset = std::max(0, (int)filtered_processes.size() - VISIBLE_ITEMS);
                    int new_offset = (int)(scroll_ratio * max_offset);
                    update_scroll_position(new_offset, max_offset);
                    draw_process_list(display, win, gc, filtered_processes);
                }
            }

            if (e.type == ButtonPress) {
                int x = e.xbutton.x;
                int y = e.xbutton.y;

                int btn_x = WINDOW_WIDTH - BUTTON_WIDTH - BUTTON_MARGIN - SCROLLBAR_WIDTH;
                int btn_y = WINDOW_HEIGHT - BUTTON_HEIGHT - BUTTON_MARGIN;
                if (selected_index != -1 &&
                    x >= btn_x && x <= btn_x + BUTTON_WIDTH &&
                    y >= btn_y && y <= btn_y + BUTTON_HEIGHT) {
                    pid_t pid = get_pid_from_process_line(filtered_processes[selected_index]);
                    if (kill(pid, SIGKILL) == 0) {
                        std::cout << "Killed process " << pid << std::endl;
                    } else {
                        perror("Failed to kill process");
                    }
                    selected_index = -1;
                    all_processes = get_detailed_processes();
                    filtered_processes.clear();
                    for (const auto& p : all_processes) {
                        if (search_text.empty() || contains_ignore_case(p, search_text))
                            filtered_processes.push_back(p);
                    }
                    draw_process_list(display, win, gc, filtered_processes);
                    continue;
                }

                if (x >= WINDOW_WIDTH - SCROLLBAR_WIDTH && 
                    y >= SEARCH_HEIGHT && 
                    y <= WINDOW_HEIGHT - BUTTON_HEIGHT - BUTTON_MARGIN) {
                    scrollbar_dragging = true;
                    scrollbar_drag_start = y;
                    int scrollbar_height = WINDOW_HEIGHT - SEARCH_HEIGHT - BUTTON_HEIGHT - BUTTON_MARGIN;
                    double scroll_ratio = (double)scroll_offset / std::max(1, (int)filtered_processes.size() - VISIBLE_ITEMS);
                    int thumb_height = std::max(30, (int)(scrollbar_height * ((double)VISIBLE_ITEMS / filtered_processes.size())));
                    scrollbar_drag_offset = y - (SEARCH_HEIGHT + (int)((scrollbar_height - thumb_height) * scroll_ratio));
                    continue;
                }

                if (x < WINDOW_WIDTH - SCROLLBAR_WIDTH && y >= SEARCH_HEIGHT) {
                    int index = (y - SEARCH_HEIGHT) / ITEM_HEIGHT + scroll_offset;
                    if (index >= 0 && index < (int)filtered_processes.size()) {
                        selected_index = index;
                        draw_process_list(display, win, gc, filtered_processes);
                    } else {
                        selected_index = -1;
                        draw_process_list(display, win, gc, filtered_processes);
                    }
                }
            }

            if (e.type == ButtonRelease) {
                scrollbar_dragging = false;
            }

            if (e.type == KeyPress) {
                KeySym keysym;
                char buf[32];
                XLookupString(&e.xkey, buf, sizeof(buf), &keysym, nullptr);

                if (search_focused) {
                    if (keysym == XK_BackSpace && !search_text.empty()) {
                        search_text.pop_back();
                    } else if (isprint(buf[0])) {
                        search_text += buf[0];
                    }
                    filtered_processes.clear();
                    for (const auto& p : all_processes) {
                        if (search_text.empty() || contains_ignore_case(p, search_text))
                            filtered_processes.push_back(p);
                    }
                    selected_index = -1;
                    scroll_offset = 0;
                    draw_process_list(display, win, gc, filtered_processes);
                } else {
                    int max_offset = std::max(0, (int)filtered_processes.size() - VISIBLE_ITEMS);
                    if (keysym == XK_Up && scroll_offset > 0) {
                        update_scroll_position(scroll_offset - 1, max_offset);
                        draw_process_list(display, win, gc, filtered_processes);
                    } else if (keysym == XK_Down && scroll_offset < max_offset) {
                        update_scroll_position(scroll_offset + 1, max_offset);
                        draw_process_list(display, win, gc, filtered_processes);
                    } else if (keysym == XK_Page_Up) {
                        update_scroll_position(scroll_offset - VISIBLE_ITEMS, max_offset);
                        draw_process_list(display, win, gc, filtered_processes);
                    } else if (keysym == XK_Page_Down) {
                        update_scroll_position(scroll_offset + VISIBLE_ITEMS, max_offset);
                        draw_process_list(display, win, gc, filtered_processes);
                    } else if (keysym == XK_Home) {
                        update_scroll_position(0, max_offset);
                        draw_process_list(display, win, gc, filtered_processes);
                    } else if (keysym == XK_End) {
                        update_scroll_position(max_offset, max_offset);
                        draw_process_list(display, win, gc, filtered_processes);
                    }
                }
            }

            if (e.type == FocusOut) {
                search_focused = false;
                draw_process_list(display, win, gc, filtered_processes);
            }

            if (e.type == FocusIn) {
                search_focused = true;
                draw_process_list(display, win, gc, filtered_processes);
            }
        }
    }

    XCloseDisplay(display);
    return 0;
}