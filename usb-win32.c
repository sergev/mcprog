/*
 * Реализация функций libusb-1.0 поверх libusb-0.1.
 * Уровень совместимости для Windows.
 */
#define libusb_device_handle	usb_dev_handle
#define libusb_context		void

int libusb_init (libusb_context **ctx)
{
	usb_init ();
	return 0;
}

libusb_device_handle *libusb_open_device_with_vid_pid (libusb_context *ctx,
	uint16_t vendor_id, uint16_t product_id)
{
	struct usb_bus *bus;
	struct usb_device *dev;

	usb_find_busses ();
	usb_find_devices ();
	for (bus = usb_get_busses(); bus; bus = bus->next) {
		for (dev = bus->devices; dev; dev = dev->next) {
			if (dev->descriptor.idVendor == vendor_id &&
			    dev->descriptor.idProduct == product_id) {
				return usb_open (dev);
			}
		}
	}
	return 0;
}

int libusb_get_configuration (libusb_device_handle *dev, int *config)
{
	*config = 0;
	return 0;
}

int libusb_set_configuration (libusb_device_handle *dev, int configuration)
{
	return usb_set_configuration (dev, configuration);
}

int libusb_claim_interface (libusb_device_handle *dev, int iface)
{
	return usb_claim_interface (dev, iface);
}

int libusb_release_interface (libusb_device_handle *dev, int iface)
{
	return usb_release_interface (dev, iface);
}

int libusb_bulk_transfer (libusb_device_handle *dev,
	unsigned char endpoint, unsigned char *data, int length,
	int *actual_length, unsigned int timeout)
{
	int ret;

	if (endpoint & 0x80)
		ret = usb_bulk_read (dev, endpoint, (char*) data,
			length, timeout);
	else
		ret = usb_bulk_write (dev, endpoint, (char*) data,
			length, timeout);
	if (ret < 0)
		return ret;
	*actual_length = ret;
	return 0;
}
