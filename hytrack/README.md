# FEMU HYSSD Monitor

A C-based terminal monitoring tool for FEMU HYSSD devices that provides real-time visualization of device status.

## Features

- Real-time monitoring of HYSSD line management data
- Visual representation of line distributions with bar graphs
- Trend analysis with sparkline graphs
- Active line tracking and display
- Clean terminal interface using ncurses

## Requirements

- Linux system with FEMU HYSSD devices
- Root privileges (for NVMe admin commands)
- Required packages:
  - gcc
  - make
  - libncurses-dev
  - nvme-cli

## Installation

### Using Installation Script

```bash
sudo ./install.sh
```

### Manual Installation

```bash
make
sudo make install
```

## Usage

Basic usage:

```bash
sudo hyssd_monitor
```

Specify a device:

```bash
sudo hyssd_monitor -d /dev/nvme1n1
```

Change update interval:

```bash
sudo hyssd_monitor -i 2.5  # Update every 2.5 seconds
```

Display help:

```bash
hyssd_monitor --help
```

## Interface Controls

- `q` - Quit the application
- `r` - Force refresh

## Displayed Information

1. **Status Overview**
   - Total lines
   - Free lines
   - Victim lines
   - Full lines
   - RZone status
   - SZone status

2. **Line Distribution**
   - Visual representation with color-coded bar graphs

3. **Top Active Lines**
   - IPC/VPC values for most active lines

4. **History Trends**
   - Time-series data for important metrics

## License

[Insert your chosen license here]

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.
