//
//  USBDeviceManager.cpp
//  usbmuxd2
//
//  Created by tihmstar on 17.08.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//

#include "USBDeviceManager.hpp"
#include <log.h>
#include <libgeneral/macros.h>
#include <Devices/USBDevice.hpp>
#include <unistd.h>
#include <string.h>

#pragma mark libusb_callback definitions
int usb_hotplug_cb(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event, void *user_data) noexcept;
void usb_get_langid_callback(struct libusb_transfer *transfer) noexcept;
void usb_get_serial_callback(struct libusb_transfer *transfer) noexcept;
void rx_callback(struct libusb_transfer *xfer) noexcept;


#pragma mark USBDeviceManager

USBDeviceManager::USBDeviceManager(Muxer *mux) : DeviceManager(mux),_usb_hotplug_cb_handle(0),_isDying(false)
{
    int ret = 0;
    bool hasCTX = false;
    cleanup([&](){ //cleanup only code
        if (hasCTX) {
            libusb_exit(NULL);
        }
    });
    
    info("USBDeviceManager libusb 1.0");
    retassure(libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG), "libusb does not support hotplug events");

    assure(!libusb_init(NULL));hasCTX = true;
    info("Registering for libusb hotplug events");
    
    retassure(!(ret = libusb_hotplug_register_callback(NULL, static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT), LIBUSB_HOTPLUG_ENUMERATE, VID_APPLE, LIBUSB_HOTPLUG_MATCH_ANY, 0, usb_hotplug_cb, this, &_usb_hotplug_cb_handle)),"ERROR: Could not register for libusb hotplug events (%d)", ret);
    hasCTX = false; //don't deinit libusb context
}

USBDeviceManager::~USBDeviceManager(){
    _isDying = true;
    stopLoop();
    
    libusb_exit(NULL);
}

void USBDeviceManager::loopEvent(){
    int ret = 0;
    retassure(!(ret = libusb_handle_events_completed(NULL, NULL)), "libusb_handle_events_completed failed: %d", ret);
}

void USBDeviceManager::stopAction() noexcept{
    libusb_hotplug_deregister_callback(NULL, _usb_hotplug_cb_handle); //trigger final event
}

void USBDeviceManager::add_constructing(uint8_t bus, uint8_t addr){
    assure(!_isDying);
    _constructing.lockMember();
    _constructing._elems.insert((uint16_t)(bus<<16) | addr);
    _constructing.unlockMember();
}

void USBDeviceManager::del_constructing(uint8_t bus, uint8_t addr){
    _constructing.lockMember();
    _constructing._elems.erase((uint16_t)(bus<<16) | addr);
    _constructing.unlockMember();
}

bool USBDeviceManager::is_constructing(uint8_t bus, uint8_t addr){
    bool ret = false;
    _constructing.addMember();
    ret = _constructing._elems.find((uint16_t)(bus<<16) | addr) != _constructing._elems.end();
    _constructing.delMember();
    return ret;
}


void USBDeviceManager::device_add(libusb_device *dev){
    int ret = 0;
    assure(!_isDying);

    struct libusb_transfer *transfer = NULL;
    unsigned char *transfer_buffer = NULL;
    libusb_device_handle *handle = NULL;
    struct libusb_config_descriptor *config = NULL;
    USBDevice *newDevice = NULL;
    
    cleanup([&]{ //cleanup only code
        if (transfer) {
            libusb_free_transfer(transfer);
        }
        safeFree(transfer_buffer);
        if (handle){
            libusb_close(handle);
        }
        if (config) {
            libusb_free_config_descriptor(config);
        }
        if (newDevice) {
            newDevice->kill();
        }
    });
    
    uint8_t bus = 0;
    uint8_t address = 0;
    int current_config = 0;
    struct libusb_device_descriptor devdesc = {};

    bus = libusb_get_bus_number(dev);
    assure((address = libusb_get_device_address(dev))>0);
    
    if (_mux->have_usb_device(bus, address) || is_constructing(bus, address)) {
        //device already found
        return;
    }

    retassure(!(ret = libusb_get_device_descriptor(dev, &devdesc)), "Could not get device descriptor for device %d-%d: %d", bus, address, ret);
    
    retassure(devdesc.idVendor == VID_APPLE, "USBDevice is not an Apple device");
    retassure(devdesc.idProduct >= PID_RANGE_LOW && devdesc.idProduct <= PID_RANGE_MAX, "USBDevice is Apple, but not PID is not in observe range");
    
    info("Found new device with v/p %04x:%04x at %d-%d", devdesc.idVendor, devdesc.idProduct, bus, address);
    
    // No blocking operation can follow: it may be run in the libusb hotplug callback and libusb will refuse any
    // blocking call
    retassure(!(ret = libusb_open(dev, &handle)),"Could not open device %d-%d: %d", bus, address, ret);
    
    retassure(!(ret = libusb_get_configuration(handle, &current_config)), "Could not get configuration for device %d-%d: %d", bus, address, ret);
    
    if (current_config != devdesc.bNumConfigurations) {
        if((ret = libusb_get_active_config_descriptor(dev, &config)) != 0) {
            notice("Could not get old configuration descriptor for device %d-%d: %d", bus, address, ret);
        } else {
            for(int j=0; j<config->bNumInterfaces; j++) {
                const struct libusb_interface_descriptor *intf = &config->interface[j].altsetting[0];
                if((ret = libusb_kernel_driver_active(handle, intf->bInterfaceNumber)) < 0) {
                    notice("Could not check kernel ownership of interface %d for device %d-%d: %d", intf->bInterfaceNumber, bus, address, ret);
                    continue;
                }
                if(ret == 1) {
                    notice("Detaching kernel driver for device %d-%d, interface %d", bus, address, intf->bInterfaceNumber);
                    if((ret = libusb_detach_kernel_driver(handle, intf->bInterfaceNumber)) < 0) {
                        warning("Could not detach kernel driver (%d), configuration change will probably fail!", ret);
                        continue;
                    }
                }
            }
        }
        notice("Setting configuration for device %d-%d, from %d to %d", bus, address, current_config, devdesc.bNumConfigurations);
        retassure(!(ret = libusb_set_configuration(handle, devdesc.bNumConfigurations)), "Could not set configuration %d for device %d-%d: %d", devdesc.bNumConfigurations, bus, address, ret);
    }
    
    retassure(!(ret = libusb_get_active_config_descriptor(dev, &config)), "Could not get configuration descriptor for device %d-%d: %d", bus, address, ret);
    
    assure(newDevice = new USBDevice(_mux));
    newDevice->_pid = devdesc.idProduct;
    newDevice->_parent = this;
    
    for(int j=0; j<config->bNumInterfaces; j++) {
        const struct libusb_interface_descriptor *intf = &config->interface[j].altsetting[0];
        if(intf->bInterfaceClass != INTERFACE_CLASS ||
           intf->bInterfaceSubClass != INTERFACE_SUBCLASS ||
           intf->bInterfaceProtocol != INTERFACE_PROTOCOL)
            continue;
        if(intf->bNumEndpoints != 2) {
            warning("Endpoint count mismatch for interface %d of device %d-%d", intf->bInterfaceNumber, bus, address);
            continue;
        }
        if((intf->endpoint[0].bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_OUT &&
           (intf->endpoint[1].bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_IN) {
            newDevice->_interface = intf->bInterfaceNumber;
            newDevice->_ep_out = intf->endpoint[0].bEndpointAddress;
            newDevice->_ep_in = intf->endpoint[1].bEndpointAddress;
            notice("Found interface %d with endpoints %02x/%02x for device %d-%d", newDevice->_interface, newDevice->_ep_out, newDevice->_ep_in, bus, address);
            goto found_device;
        } else if((intf->endpoint[1].bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_OUT &&
                  (intf->endpoint[0].bEndpointAddress & 0x80) == LIBUSB_ENDPOINT_IN) {
            newDevice->_interface = intf->bInterfaceNumber;
            newDevice->_ep_out = intf->endpoint[1].bEndpointAddress;
            newDevice->_ep_in = intf->endpoint[0].bEndpointAddress;
            warning("Found interface %d with swapped endpoints %02x/%02x for device %d-%d", newDevice->_interface, newDevice->_ep_out, newDevice->_ep_in, bus, address);
            goto found_device;
        } else {
            warning("Endpoint type mismatch for interface %d of device %d-%d", intf->bInterfaceNumber, bus, address);
        }
    }
    
    reterror("Could not find a suitable USB interface for device %d-%d", bus, address);
found_device:
    
    retassure(!(ret = libusb_claim_interface(handle, newDevice->_interface)), "Could not claim interface %d for device %d-%d: %d", newDevice->_interface, bus, address, ret);
    
    retassure(transfer = libusb_alloc_transfer(0), "Failed to allocate transfer for device %d-%d: %d", bus, address, ret);
    
    newDevice->_serial[0] = 0;
    newDevice->_bus = bus;
    newDevice->_address = address;
    newDevice->_devdesc = devdesc;
    newDevice->_speed = 480000000;
    newDevice->_usbdev = handle; handle = NULL; //transfering ownership here!
    newDevice->_wMaxPacketSize = libusb_get_max_packet_size(dev, newDevice->_ep_out);
    
    if (newDevice->_wMaxPacketSize <= 0) {
        error("Could not determine wMaxPacketSize for device %d-%d, setting to 64", newDevice->_bus, newDevice->_address);
        newDevice->_wMaxPacketSize = 64;
    } else {
        notice("Using wMaxPacketSize=%d for device %d-%d", newDevice->_wMaxPacketSize, newDevice->_bus, newDevice->_address);
    }
    
    switch (libusb_get_device_speed(dev)) {
        case LIBUSB_SPEED_LOW:
            newDevice->_speed = 1500000;
            break;
        case LIBUSB_SPEED_FULL:
            newDevice->_speed = 12000000;
            break;
        case LIBUSB_SPEED_SUPER:
            newDevice->_speed = 5000000000;
            break;
        case LIBUSB_SPEED_HIGH:
        case LIBUSB_SPEED_UNKNOWN:
        default:
            newDevice->_speed = 480000000;
            break;
    }
    
    
    info("USB Speed is %g MBit/s for device %d-%d", (double)(newDevice->_speed / 1000000.0), newDevice->_bus, newDevice->_address);
    
    
    /**
     * From libusb:
     *     Asking for the zero'th index is special - it returns a string
     *     descriptor that contains all the language IDs supported by the
     *     device.
     **/
    retassure(transfer_buffer = (unsigned char *)malloc(1024 + LIBUSB_CONTROL_SETUP_SIZE + 8), "Failed to allocate transfer buffer for device %d-%d: %d", bus, address, ret);
    memset(transfer_buffer, '\0', 1024 + LIBUSB_CONTROL_SETUP_SIZE + 8);
    
    
    libusb_fill_control_setup(transfer_buffer, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR, LIBUSB_DT_STRING << 8, 0, 1024 + LIBUSB_CONTROL_SETUP_SIZE);
    libusb_fill_control_transfer(transfer, newDevice->_usbdev, transfer_buffer, usb_get_langid_callback, newDevice, 1000);
    newDevice = NULL; //transfer ownership to Muxer if transfer succeeds, otherwise free newDevice (in callback)
    
    retassure(!(ret = libusb_submit_transfer(transfer)), "Could not request transfer for device %d-%d (%d)", newDevice->_bus, newDevice->_address, ret);
    
    transfer = NULL; //transfer in process, needs to be freed by callback
    transfer_buffer = NULL; //transfer in process, needs to be freed by callback
    
    add_constructing(bus, address);
}

// Start a read-callback loop for this device
void usb_start_rx_loop(USBDevice *dev){
    int ret = 0;
    void *buf = NULL;
    struct libusb_transfer *xfer = NULL;
    cleanup([&](){ //cleanup only code
        safeFree(buf);
        if (xfer) {
            dev->_rx_xfers.lockMember();
            dev->_rx_xfers._elems.erase(xfer);
            dev->_rx_xfers.unlockMember();
            safeFree(xfer->buffer);
            libusb_free_transfer(xfer);
        }
    });
    
    assure(buf = malloc(USB_MRU));
    assure(xfer = libusb_alloc_transfer(0));
    
    libusb_fill_bulk_transfer(xfer, dev->_usbdev, dev->_ep_in, (unsigned char *)buf, USB_MRU, rx_callback, dev, 0);
    buf = NULL; //owned by xfer now

    dev->_rx_xfers.lockMember();
    dev->_rx_xfers._elems.insert(xfer);//transfer ownsership of transfer to device
    dev->_rx_xfers.unlockMember();
    retassure(!((ret = libusb_submit_transfer(xfer)),ret),"Failed to submit RX transfer to device %d-%d: %d", dev->_bus, dev->_address, ret);
    xfer = NULL;
}

#pragma mark libusb_callback implementations

int usb_hotplug_cb(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event, void *user_data) noexcept{
    int err = 0;
    USBDeviceManager *devmngr = (USBDeviceManager *)user_data;
    switch (event) {
        case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED:
            try {
                devmngr->device_add(device);
            } catch (tihmstar::exception &e) {
                uint8_t bus = libusb_get_bus_number(device);
                uint8_t address = libusb_get_device_address(device);
                creterror("failed to add device on bus=0x%02x address=0x%02x error=%s code=%d",bus,address,e.what(),e.code());
            }
            break;
        case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT:
        {
            uint8_t bus = libusb_get_bus_number(device);
            uint8_t address = libusb_get_device_address(device);
            devmngr->_mux->delete_device_async(bus, address);
            break;
        }
        default:
            error("Unhandled event %d", event);
            break;
    }
error:
    return 0;
}

void usb_get_langid_callback(struct libusb_transfer *transfer) noexcept{
    int err = 0;
    int ret = 0;
    USBDevice *usbdev = (USBDevice *)transfer->user_data;
    unsigned char *data = NULL;
    uint16_t langid = 0;
    
    transfer->flags |= LIBUSB_TRANSFER_FREE_BUFFER;
    cretassure(transfer->status == LIBUSB_TRANSFER_COMPLETED, "Failed to request lang ID for device %d-%d (%i)", usbdev->_bus, usbdev->_address, transfer->status);
    
    data = libusb_control_transfer_get_data(transfer); //error-free function
    langid = (uint16_t)(data[2] | (data[3] << 8));
    info("Got lang ID %u for device %d-%d", langid, usbdev->_bus, usbdev->_address);
    
    /* re-use the same transfer */
    libusb_fill_control_setup(transfer->buffer, LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
                              (uint16_t)((LIBUSB_DT_STRING << 8) | usbdev->_devdesc.iSerialNumber),
                              langid, 1024 + LIBUSB_CONTROL_SETUP_SIZE);
    libusb_fill_control_transfer(transfer, usbdev->_usbdev, transfer->buffer, usb_get_serial_callback, usbdev, 1000);
    
    cretassure((ret = libusb_submit_transfer(transfer)) >= 0, "Could not request transfer for device %d-%d (%d)", usbdev->_bus, usbdev->_address, ret);
    
error:
    if (err) {
        usbdev->_parent->del_constructing(usbdev->_bus, usbdev->_address);
        //only free on error
        if (transfer)
            libusb_free_transfer(transfer);transfer = NULL;
        //at this point we are the only owner of usbdev
        //if anything goes wrong, this is the last chance to delete the object without leaking it
        usbdev->kill();
    }
}


void usb_get_serial_callback(struct libusb_transfer *transfer) noexcept{
    int err = 0;
    USBDevice *usbdev = (USBDevice *)transfer->user_data;
    unsigned char *data = NULL;
    std::shared_ptr<Muxer> mux = NULL;
    int rx_loops = 0;
    unsigned int di = 0, si = 0;
    
    cretassure(transfer->status == LIBUSB_TRANSFER_COMPLETED, "Failed to request serial for device %d-%d (%i)", usbdev->_bus, usbdev->_address, transfer->status);
    
    /* De-unicode, taken from libusb */
    cassure(data = libusb_control_transfer_get_data(transfer));
    
    for (di = 0, si = 2; si < data[0] && di < sizeof(usbdev->_serial)-1; si += 2) {
        if ((data[si] & 0x80) || (data[si + 1])) /* non-ASCII */
            usbdev->_serial[di++] = '?';
        else if (data[si] == '\0')
            break;
        else
            usbdev->_serial[di++] = data[si];
    }
    //should already be zero terminated at the correct offset (hopefully)
    //just doing sanity zero termination
    usbdev->_serial[sizeof(usbdev->_serial)-1] = 0;
        
    /* new style UDID: add hyphen between first 8 and following 16 digits */
    if (di == 24) {
        memmove(&usbdev->_serial[9], &usbdev->_serial[8], 16);
        usbdev->_serial[8] = '-';
        usbdev->_serial[di+1] = '\0';
    }

    info("Got serial '%s' for device %d-%d (%p)", usbdev->_serial, usbdev->_bus, usbdev->_address,usbdev);


    // Spin up NUM_RX_LOOPS parallel usb data retrieval loops
    // Old usbmuxds used only 1 rx loop, but that leaves the
    // USB port sleeping most of the time
    
    for (rx_loops = 0; rx_loops < NUM_RX_LOOPS; rx_loops++) {
        try {
            usb_start_rx_loop(usbdev);
        } catch (tihmstar::exception &e) {
            warning("Failed to start RX loop number %d", NUM_RX_LOOPS - rx_loops);
        }
    }
    
    // Ensure we have at least 1 RX loop going
    cretassure(rx_loops, "Failed to start any RX loop for device %d-%d", usbdev->_bus, usbdev->_address);
    if (rx_loops != NUM_RX_LOOPS) {
        warning("Failed to start all %d RX loops. Going on with %d loops. This may have negative impact on device read speed.", NUM_RX_LOOPS, rx_loops);
    } else {
        debug("All %d RX loops started successfully", NUM_RX_LOOPS);
    }
    
    try {
        usbdev->mux_init();
    } catch (tihmstar::exception &e) {
        creterror("failed to mux_init usbdev=%s error=%s code=%d",usbdev->_serial,e.what(),e.code());
    }
    
error:
    usbdev->_parent->del_constructing(usbdev->_bus, usbdev->_address);
    usbdev = NULL; //transfered ownership to Muxer
    libusb_free_transfer(transfer);
    
    if (usbdev) { //we are the only owner of usbdev, if something goes wrong, free the object
        usbdev->kill();
    }
    if (err) {
        error("usb_get_serial_callback err=%d",err);
    }
}

void rx_callback(struct libusb_transfer *xfer) noexcept{
    int err = 0;
    USBDevice *dev = (USBDevice *)xfer->user_data;
    
    debug("RX callback dev %d-%d len %d status %d", dev->_bus, dev->_address, xfer->actual_length, xfer->status);
    if(xfer->status == LIBUSB_TRANSFER_COMPLETED) {
        try {
            dev->device_data_input(xfer->buffer, xfer->actual_length);
        } catch (tihmstar::exception &e) {
            creterror("failed to device_data_input usbdev=%s error=%s code=%d",dev->_serial,e.what(),e.code());
        }
        libusb_submit_transfer(xfer);
        return;
    }
    switch(xfer->status) {
        case LIBUSB_TRANSFER_COMPLETED: //shut up compiler
        case LIBUSB_TRANSFER_ERROR:
            // funny, this happens when we disconnect the device while waiting for a transfer, sometimes
            info("Device %d-%d RX aborted due to error or disconnect", dev->_bus, dev->_address);
            break;
        case LIBUSB_TRANSFER_TIMED_OUT:
            error("RX transfer timed out for device %d-%d", dev->_bus, dev->_address);
            break;
        case LIBUSB_TRANSFER_CANCELLED:
            debug("Device %d-%d RX transfer cancelled", dev->_bus, dev->_address);
            break;
        case LIBUSB_TRANSFER_STALL:
            error("RX transfer stalled for device %d-%d", dev->_bus, dev->_address);
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            // other times, this happens, and also even when we abort the transfer after device removal
            info("Device %d-%d RX aborted due to disconnect", dev->_bus, dev->_address);
            break;
        case LIBUSB_TRANSFER_OVERFLOW:
            error("RX transfer overflow for device %d-%d", dev->_bus, dev->_address);
            break;
            // and nothing happens (this never gets called) if the device is freed after a disconnect! (bad)
        default:
            // this should never be reached.
            break;
    }
    
error:
    //remove transfer
    dev->_rx_xfers.lockMember();
    dev->_rx_xfers._elems.erase(xfer);
    dev->_rx_xfers.unlockMember();
    
    debug("freing rx xfer (%p) for USBDevice(%p)",xfer,dev);
    safeFree(xfer->buffer);
    libusb_free_transfer(xfer);
    dev->kill();
}
