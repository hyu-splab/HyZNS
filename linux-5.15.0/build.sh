#!/bin/bash

# Log directory setup
LOG_DIR="kernel_logs"
LOG_FILE="$LOG_DIR/kernel_compile_$(date +%Y%m%d_%H%M%S).log"

# Print usage
show_usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -c, --clean     remove all previous log files"
    echo "  -h, --help      show this help"
    echo "  -m, --menuconfig    run the kernel config menu"
    echo "  -d, --disable-keys  disable SYSTEM_TRUSTED_KEYS"
    echo "Examples:"
    echo "  $0                  # normal run"
    echo "  $0 --clean         # clear old logs, then run"
    echo "  $0 --menuconfig    # run with the kernel config menu"
    echo "  $0 --disable-keys  # disable SYSTEM_TRUSTED_KEYS"
}

# Create the log directory if needed
init_log_directory() {
    if [ ! -d "$LOG_DIR" ]; then
        mkdir -p "$LOG_DIR"
        echo "Created log directory: $LOG_DIR"
    fi
}

# Remove old logs
clean_logs() {
    if [ -d "$LOG_DIR" ]; then
        echo "Removing previous log files..."
        rm -f "$LOG_DIR"/*.log
        echo "Log cleanup done."
    fi
}

# Append a timestamped message to the log
log_message() {
    local message="$1"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $message" | tee -a "$LOG_FILE"
}

# Abort on a failed step
handle_error() {
    local step="$1"
    log_message "Error: step '$step' failed."
    log_message "See the log file: $LOG_FILE"
    exit 1
}

# Prepare the kernel .config
prepare_kernel_config() {
    log_message "Preparing kernel config..."
    if ! cp /boot/config-$(uname -r) .config 2>&1 | tee -a "$LOG_FILE"; then
        handle_error "copy kernel config"
    fi
    log_message "Kernel config ready"
}

# Run the kernel config menu
run_menuconfig() {
    log_message "Running menuconfig..."
    if ! make menuconfig 2>&1 | tee -a "$LOG_FILE"; then
        handle_error "menuconfig"
    fi
    log_message "menuconfig done"
}

# Disable SYSTEM_TRUSTED_KEYS
disable_trusted_keys() {
    log_message "Disabling SYSTEM_TRUSTED_KEYS..."
    if ! scripts/config --disable SYSTEM_TRUSTED_KEYS 2>&1 | tee -a "$LOG_FILE"; then
        handle_error "disable SYSTEM_TRUSTED_KEYS"
    fi
    log_message "SYSTEM_TRUSTED_KEYS disabled"
}

# Parse command-line arguments
while [ "$1" != "" ]; do
    case $1 in
        -c | --clean )        clean_option=1
                             ;;
        -m | --menuconfig )   menuconfig_option=1
                             ;;
        -d | --disable-keys ) disable_keys_option=1
                             ;;
        -h | --help )         show_usage
                             exit 0
                             ;;
        * )                  show_usage
                             exit 1
    esac
    shift
done

# Prepare the log directory
init_log_directory

# Optionally clear old logs
if [ "$clean_option" = "1" ]; then
    clean_logs
fi

# Announce start
log_message "Starting kernel build..."
log_message "Log file: $LOG_FILE"

# Prepare kernel config
prepare_kernel_config

# Optionally run menuconfig
if [ "$menuconfig_option" = "1" ]; then
    run_menuconfig
fi

# Optionally disable SYSTEM_TRUSTED_KEYS
if [ "$disable_keys_option" = "1" ]; then
    disable_trusted_keys
fi

# Step 1: build the kernel
log_message "Compiling kernel..."
if ! make -j$(nproc) 2>&1 | tee -a "$LOG_FILE"; then
    handle_error "kernel compile"
fi
log_message "Kernel compile done"

# Step 2: install headers
log_message "Installing headers..."
if ! sudo make headers_install INSTALL_HDR_PATH=/usr 2>&1 | tee -a "$LOG_FILE"; then
    handle_error "headers install"
fi
log_message "Headers installed"

# Step 3: install modules
log_message "Installing modules..."
if ! make INSTALL_MOD_STRIP=1 modules_install -j$(nproc) 2>&1 | tee -a "$LOG_FILE"; then
    handle_error "modules install"
fi
log_message "Modules installed"

# Step 4: install the kernel
log_message "Installing kernel..."
if ! make install -j$(nproc) 2>&1 | tee -a "$LOG_FILE"; then
    handle_error "kernel install"
fi
log_message "Kernel installed"

# Step 5: update initramfs
log_message "Updating initramfs..."
if ! sudo update-initramfs -u -k $(uname -r) 2>&1 | tee -a "$LOG_FILE"; then
    handle_error "initramfs update"
fi
log_message "initramfs updated"

# Step 6: update GRUB
log_message "Updating GRUB..."
if ! sudo update-grub 2>&1 | tee -a "$LOG_FILE"; then
    handle_error "GRUB update"
fi
log_message "GRUB updated"

log_message "All steps completed successfully."
