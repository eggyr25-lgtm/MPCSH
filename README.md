# MPCSH 1.6 - Modern Enhanced Shell

## Overview

MPCSH 1.6 is the next generation of the MarekPC Shell, featuring enhanced window management, addon support, and modern GUI integration.

## Key Features

### New in MPCSH 1.6:

✅ **Window Management**
- `windowlaunch --persist <title> <command>` - Launch persistent windows
- `windowterminate <id>` - Terminate specific windows
- `windowhide <id>`, `windowshow <id>`, `windowminimize <id>`, `windowmaximize <id>` - Window control
- **F3** - Window picker for selecting windows

✅ **Configuration System**
- `windowlaunch --configure --accent-color <color>` - Set window accent color
- `windowlaunch --configure --blur-level <level>` - Set glass blur level (0-100)
- `windowlaunch --configure --color-loop` - Enable color cycling
- `windowlaunch --configure --colormax <level>` - Set 64-color mode

✅ **Advanced Window Features**
- Resizable windows
- Minimize/maximize/fullscreen support
- Window ID system (wlid) for targeting
- Taskbar with window switching
- LXQt integration

✅ **Addon System**
- `.mpcshaddon` file format for extensions
- Sample addon: `sample-addon`
- Addon command execution

✅ **Modern GUI**
- GTK4-based interface
- Bottom taskbar
- Aero glass effects
- Professional window styling

## Quick Start

### Installation

```bash
# Build MPCSH 1.6
cd MPCSH1.6
make

# Install to system
sudo make install
```

### Basic Usage

```bash
# Launch MPCSH 1.6
mpcsh_1_6

# Help
Press 'h' for help
```

### Core Commands

**Traditional MPCSH Commands:**
```bash
pwd                    # Print current directory
cd [path]              # Change directory
ls [options]           # List files and directories
exit                   # Exit MPCSH
```

**New Window Management:**
```bash
# Launch a new window (persists)
windowlaunch --persist "My Window" "/usr/bin/terminal"

# Launch window with custom settings
windowlaunch --title "Firefox" --command "/usr/bin/firefox" --width 1024 --height 768

# Terminate window by ID
windowterminate 42

# Hide/show window
windowhide 42
windowshow 42

# Minimize/maximize window
windowminimize 42
windowmaximize 42

# Window picker (F3 key)
# Press 'F3' or use windowpick command
```

**Configuration:**
```bash
# Configure window appearance
windowlaunch --configure --accent-color "#FF00FF" --blur-level 75 --color-loop

# Set 64-color mode
windowlaunch --configure --colormax 64
```

**Addon System:**
```bash
# List available addons
addonlist

# Load addon
addonload sample-addon

# Unload addon
addonunload sample-addon

# Execute addon command
addon sample-addon command-name
```

## Files Created

### Project Structure:
```
MPCSH1.6/
├── src/
│   ├── main.c          # Main application
│   └── ...            # Additional source files
├── include/
│   └── mpcsh1.6.h     # Header files
├── Makefile           # Build script
└── README.md          # This file
```

### Generated Files:
- `MPCSH1.6/src/main.o` - Compiled object file
- `MPCSH1.6/mpcsh_1_6` - Executable binary

## Building

### Prerequisites
- GTK4 development libraries
- GTK4 GDK-Pixbuf library
- Standard C compiler

### Build Commands

```bash
cd MPCSH1.6

# Build with default settings
make

# Build with additional libraries
make CFLAGS="-O2 -Wall" GTK_LIBS="$(pkg-config --libs gtk4 gdk-pixbuf-2.0)"

# Install to system
make install
```

### Clean Build

```bash
make clean
```

## Configuration

### Default Configuration

MPCSH 1.6 comes with sensible defaults:

- **Default accent color:** Purple (`#FF00FF`)
- **Default blur level:** 50%
- **Color loops:** Disabled
- **Colormax mode:** Disabled (full color)
- **Window size:** 400x300 pixels

### Configuration Files

Add `/home/marek/.config/mpcsh1.6/` directory and create:

- `config.json` - Main configuration
- `addons/` - Addon definitions
- `themes/` - Color themes

Example `config.json`:
```json
{
  "default_accent_color": "#FF00FF",
  "default_blur_level": 50,
  "color_loop_enabled": false,
  "enable_aero": true,
  "colormax_mode": 64
}
```

## Key Features Demonstration

### 1. Window Management

```bash
# Launch a browser window
windowlaunch --persist "Browser Window" "/usr/bin/firefox" --width 1024 --height 768 --accent-color "#00FF00"

# Launch terminal with custom settings
windowlaunch --title "Terminal" --command "xterm" --width 600 --height 400 --blur-level 30
```

### 2. Addon System

Create a `.mpcshaddon` file:
```json
{
  "name": "quicklaunch-addon",
  "version": "1.0.0",
  "description": "Quick launch common applications",
  "author": "Developer",
  "commands": [
    {
      "name": "term",
      "description": "Launch terminal",
      "action": "windowlaunch --title 'Terminal' --command '/usr/bin/terminal'"
    },
    {
      "name": "browser",
      "description": "Launch browser",
      "action": "windowlaunch --title 'Browser' --command '/usr/bin/firefox'"
    }
  ]
}
```

### 3. Configuration

```bash
# Change global accent color
set_accent_color "#FF0000"

# Enable color cycling
toggle_color_loop

# Set blur level
set_blur_level 80

# Save configuration
save_configuration
```

## Troubleshooting

### Common Issues

**1. GTK4 not found**
```bash
# Install GTK4 development packages
sudo apt install libgtk-4-dev libgtk-4-0

# On Ubuntu/Debian
# On Fedora/CentOS
```

**2. Build errors**
```bash
# Try with verbose output
make CFLAGS="-O2 -Wall -g"

# Check for required libraries
pkg-config --libs --cflags gtk4 gdk-pixbuf-2.0
```

**3. Runtime errors**
```bash
# Check for missing dependencies
ldd mpcsh_1_6
```

### Getting Help

- Press `h` in MPCSH 1.6 for help
- Check the documentation in this directory
- Report issues on GitHub

## Future Enhancements

### Planned Features:
- Multi-monitor support
- Window snap layouts
- Custom themes and skins
- Advanced window animations
- Integration with system tray
- Cloud storage for window sessions
- AI-powered window management

### Upcoming Versions:
- **MPCSH 1.7:** AI integration, smart window management
- **MPCSH 2.0:** Complete rewrite with new architecture

## About

MPCSH 1.6 is developed by MarekPC as an enhanced version of the original MPCSH shell. This project demonstrates advanced GUI programming, window management, and software architecture.

**License:** Proprietary
**Version:** 1.6.0
**Author:** MarekPC Team

For more information, visit the project repository or contact the development team.
