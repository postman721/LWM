### Lightweight Window Manager (LWM)

LWM is a minimalistic window manager for the X Window System built with XCB, designed for simplicity and efficiency. It provides basic window management features like moving, resizing, maximizing, and closing windows, alongside additional controls for window ordering.

![Image](https://github.com/user-attachments/assets/7f54e7ca-0525-4c04-b310-0557bdb39625)

### Features
Key Bindings:
 *  - Alt+Mouse Left/Right for window move/resize (with edge snapping).
 *  - Alt+F toggles fullscreen.
 *  - Alt+E closes focused window (sends WM_DELETE_WINDOW if available).
 *  - Alt+Q shows an exit confirmation dialog.
 *  - Alt+R shows a "Runner" prompt (with a larger font).
 *  - Alt+Tab cycles through windows.
 *  - Alt+I shows a help dialog with key bindings.
 *  - Alt+M minimizes a window.
 *  - Alt+N restores all minimized windows.
 *  - Focus follows mouse.

    Additional mouse controls:
        Alt + Left Mouse Button: Drag window to move it.
        Alt + Right Mouse Button: Resize window.

###### Notice. When pressing ALT+I the info window closes with ESC.

### Customization

    Focus Follows Mouse:
    Uncomment the #define FOCUS_FOLLOWS_MOUSE directive in the source code if you prefer "sloppy focus".

### Mouse Interactions

    Left-click and drag on title bar: Move the window.
    Right-click and drag on title bar: Resize the window.
    Close button: Closes the window.
    Maximize and Minimize buttons: Perform respective actions on the window.

### Prerequisites

    X11: Requires a working X11 environment.
    sudo apt install make

Compilation with Makefile: sudo make all:   - Checks dependencies. 
											- Compiles lwm.cpp and moves it to /usr/bin 
											- Makes lightdm configuration,
### Usage

    Running the window manager with .xinitrc (startx) or simililar, lwm is the last entry of the file:
    
		exec lwm

Lightdm usage is also an option and it is integrated into Makefile.
Xbindkeys is fully supported as well.

### Uninstall LWM
	
		sudo make clean
