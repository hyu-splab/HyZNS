#!/bin/bash
# HYTRACK (HYSSD/BBSSD/Multi-Device) Monitor Installation Script
# This script installs the monitor directly from the current directory

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check permissions
if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}This script must be run with root privileges.${NC}"
    echo "Please run with: sudo $0"
    exit 1
fi

echo -e "${BLUE}=== HYTRACK (HYSSD/BBSSD/Multi-Device) Monitor Installation Starting ===${NC}"

# Make sure the required directories exist
echo -e "${YELLOW}Ensuring directory structure exists...${NC}"
mkdir -p obj/core obj/common obj/modes obj/views

# Check if source files exist
for dir in include include/core include/common include/modes include/views src src/core src/common src/modes src/views; do
    if [ ! -d "$dir" ]; then
        echo -e "${RED}Error: Directory $dir is missing.${NC}"
        echo -e "${YELLOW}Please make sure you are in the correct directory.${NC}"
        exit 1
    fi
done

# Check and install required packages
echo -e "${YELLOW}Checking required packages...${NC}"
packages=(gcc make libncurses-dev nvme-cli)
need_update=0

for package in "${packages[@]}"; do
    if ! dpkg -s "$package" >/dev/null 2>&1; then
        echo -e "${YELLOW}Package $package is required. Installing...${NC}"
        need_update=1
    fi
done

if [ $need_update -eq 1 ]; then
    echo -e "${YELLOW}Updating and installing packages...${NC}"
    apt-get update
    apt-get install -y "${packages[@]}"
else
    echo -e "${GREEN}All required packages are installed.${NC}"
fi

# Compile
echo -e "${YELLOW}Compiling...${NC}"
make clean
if ! make; then
    echo -e "${RED}Compilation error.${NC}"
    exit 1
fi

# Install
echo -e "${YELLOW}Installing...${NC}"
if ! make install; then
    echo -e "${RED}Installation error.${NC}"
    exit 1
fi

# Completion message
echo -e "${GREEN}=== HYTRACK Monitor Installation Complete ===${NC}"
echo -e "You can run the monitor with: ${BLUE}hytrack${NC}"
echo -e "For options, run: ${BLUE}hytrack --help${NC}"
echo
echo -e "${YELLOW}Available command line options:${NC}"
echo -e "  -d, --device=DEV     Device to monitor (default: /dev/nvme0n1)"
echo -e "  -2, --device2=DEV    Second device for multi-device mode"
echo -e "  -m, --mode=MODE      Operating mode: hyssd, bbssd, multi (default: hyssd)"
echo -e "  -i, --interval=SEC   Update interval in seconds (default: 1.0)"
echo -e "  -v, --view=NUM       Starting view number (default: 0)"
echo -e "  -c, --no-cursor      Hide cursor (default)"
echo -e "  -D, --debug          Enable debug mode"
echo -e "  -L, --start-log      Auto-start logging (switches to log view)"
echo -e "  -M, --log-mode=MODE  Log mode: aggregate, individual (default: aggregate)"
echo -e "  -O, --log-dir=DIR    Directory to store log files (default: logs)"
echo -e "  -h, --help           Show help message"
echo
echo -e "${YELLOW}Example usage:${NC}"
echo -e "  ${BLUE}hytrack -m bbssd -d /dev/nvme0n1                  # BBSSD mode${NC}" 
echo -e "  ${BLUE}hytrack -m multi -d /dev/nvme0n1 -2 /dev/nvme1n1  # Multi-device mode${NC}"
echo -e "  ${BLUE}hytrack -d /dev/nvme2n1                           # HYSSD mode${NC}"
echo -e "  ${BLUE}hytrack -L -M individual                          # Auto-start logging in individual mode${NC}"
echo -e "  ${BLUE}hytrack -L -O /mnt/tmpfs                          # Auto-start logging to custom directory${NC}"
echo
echo -e "${YELLOW}Note: This monitoring tool may require root privileges.${NC}"