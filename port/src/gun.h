/* Mouse as a Sega Menacer on port 2. SDL-free: main.c feeds it SDL events, scripts drive it.
 * Coordinates are game pixels of the video frame (0..319 x 0..223 for the 320x224 mode). */
#ifndef T2_GUN_H
#define T2_GUN_H
#include <stdbool.h>

enum { MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_MIDDLE = 3 };

void gun_set_enabled(bool on);          /* --no-mouse: everything below is a no-op, port 2 is empty */
bool gun_enabled(void);
/* which Menacer button the right mouse button is: 'A', 'B' or 'C' */
void gun_set_right(char which);

void gun_move(int gx, int gy);          /* the mouse is over the game image at this pixel */
void gun_leave(void);                   /* the mouse is off the game image: the gun points away */
void gun_button(int button, bool down); /* MOUSE_LEFT = trigger, MOUSE_RIGHT = configurable, MOUSE_MIDDLE = Start */

/* position and Menacer button mask (GUN_* of emu.h) as last set */
void gun_state(int *gx, int *gy, bool *off, unsigned *buttons);

/* once per emulated frame, before it runs: hand the state to the core */
void gun_frame(void);

/* window pixel -> game pixel for an integer-scaled, centred (letterboxed) image:
 * out_w x out_h = renderer output size in pixels, tex_w x tex_h = game image.
 * Returns false when (wx, wy) is outside the image. */
bool gun_map_window(int wx, int wy, int out_w, int out_h, int tex_w, int tex_h, int *gx, int *gy);

#endif
