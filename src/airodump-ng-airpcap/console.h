#ifndef _CONSOLE_H
#define _CONSOLE_H

#define BLACK_WHITE        \
    FOREGROUND_INTENSITY | \
    FOREGROUND_BLUE      | \
    FOREGROUND_GREEN     | \
    FOREGROUND_RED

#define TEXTATTR BLACK_WHITE

void set_text_color( int col );
void set_cursor_pos( int x, int y );
void clear_console( int *ws_row, int *ws_col );
void set_console_size( int ws_row, int ws_col );
void set_console_icon( char *title );

#endif /* console.h */
