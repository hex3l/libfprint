/*
 * FpContext - A FPrint context
 * Copyright (C) 2019 Benjamin Berg <bberg@redhat.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "context"
#include <fpi-log.h>

#include "fpi-context.h"
#include "fpi-device.h"
#include <gusb.h>
#include <stdio.h>

#include <config.h>

#ifdef HAVE_UDEV
#include <gudev/gudev.h>
#endif

/**
 * SECTION: fp-context
 * @title: FpContext
 * @short_description: Discover fingerprint devices
 *
 * The #FpContext allows you to discover fingerprint scanning hardware. This
 * is the starting point when integrating libfprint into your software.
 *
 * The <link linkend="device-added">device-added</link> and device-removed signals allow you to handle devices
 * that may be hotplugged at runtime.
 */

typedef struct
{
  GUsbContext  *usb_ctx;
  GCancellable *cancellable;

  GSList       *sources;

  gint          pending_devices;
  gboolean      enumerated;

  GArray       *drivers;
  GPtrArray    *devices;
} FpContextPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (FpContext, fp_context, G_TYPE_OBJECT)

enum {
  DEVICE_ADDED_SIGNAL,
  DEVICE_REMOVED_SIGNAL,
  LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

static const char *
get_drivers_whitelist_env (void)
{
  return g_getenv ("FP_DRIVERS_WHITELIST");
}

static gboolean
is_driver_allowed (const gchar *driver)
{
  g_auto(GStrv) whitelisted_drivers = NULL;
  const char *fp_drivers_whitelist_env;
  int i;

  g_return_val_if_fail (driver, TRUE);

  fp_drivers_whitelist_env = get_drivers_whitelist_env ();

  if (!fp_drivers_whitelist_env)
    return TRUE;

  whitelisted_drivers = g_strsplit (fp_drivers_whitelist_env, ":", -1);

  for (i = 0; whitelisted_drivers[i]; ++i)
    if (g_strcmp0 (driver, whitelisted_drivers[i]) == 0)
      return TRUE;

  return FALSE;
}

typedef struct
{
  FpContext *context;
  FpDevice  *device;
  GSource   *source;
} RemoveDeviceData;

static gboolean
remove_device_idle_cb (RemoveDeviceData *data)
{
  FpContextPrivate *priv = fp_context_get_instance_private (data->context);
  guint idx = 0;

  g_return_val_if_fail (g_ptr_array_find (priv->devices, data->device, &idx), G_SOURCE_REMOVE);

  g_signal_emit (data->context, signals[DEVICE_REMOVED_SIGNAL], 0, data->device);
  g_ptr_array_remove_index_fast (priv->devices, idx);

  return G_SOURCE_REMOVE;
}

static void
remove_device_data_free (RemoveDeviceData *data)
{
  FpContextPrivate *priv = fp_context_get_instance_private (data->context);

  priv->sources = g_slist_remove (priv->sources, data->source);
  g_free (data);
}

static void
remove_device (FpContext *context, FpDevice *device)
{
  g_autoptr(GSource) source = NULL;
  FpContextPrivate *priv = fp_context_get_instance_private (context);
  RemoveDeviceData *data;

  data = g_new (RemoveDeviceData, 1);
  data->context = context;
  data->device = device;

  source = data->source = g_idle_source_new ();
  g_source_set_callback (source,
                         G_SOURCE_FUNC (remove_device_idle_cb), data,
                         (GDestroyNotify) remove_device_data_free);
  g_source_attach (source, g_main_context_get_thread_default ());

  priv->sources = g_slist_prepend (priv->sources, source);
}

static void
device_remove_on_notify_open_cb (FpContext *context, GParamSpec *pspec, FpDevice *device)
{
  remove_device (context, device);
}

static void
device_removed_cb (FpContext *context, FpDevice *device)
{
  gboolean open = FALSE;

  g_object_get (device, "open", &open, NULL);

  /* Wait for device close if the device is currently still open. */
  if (open)
    {
      g_signal_connect_object (device, "notify::open",
                               (GCallback) device_remove_on_notify_open_cb,
                               context,
                               G_CONNECT_SWAPPED);
    }
  else
    {
      remove_device (context, device);
    }
}

static void
async_device_init_done_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  FpDevice *device;
  FpContext *context;
  FpContextPrivate *priv;

  device = FP_DEVICE (g_async_initable_new_finish (G_ASYNC_INITABLE (source_object),
                                                   res, &error));
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  context = FP_CONTEXT (user_data);
  priv = fp_context_get_instance_private (context);
  priv->pending_devices--;

  if (error)
    {
      g_message ("Ignoring device due to initialization error: %s", error->message);
      return;
    }

  g_ptr_array_add (priv->devices, device);

  g_signal_connect_object (device, "removed",
                           (GCallback) device_removed_cb,
                           context,
                           G_CONNECT_SWAPPED);

  g_signal_emit (context, signals[DEVICE_ADDED_SIGNAL], 0, device);
}

static void
usb_device_added_cb (FpContext *self, GUsbDevice *device, GUsbContext *usb_ctx)
{
  FpContextPrivate *priv = fp_context_get_instance_private (self);
  GType found_driver = G_TYPE_NONE;
  const FpIdEntry *found_entry = NULL;
  gint found_score = 0;
  gint i;
  guint16 pid, vid;

  pid = g_usb_device_get_pid (device);
  vid = g_usb_device_get_vid (device);

  /* Find the best driver to handle this USB device. */
  for (i = 0; i < priv->drivers->len; i++)
    {
      GType driver = g_array_index (priv->drivers, GType, i);
      g_autoptr(FpDeviceClass) cls = g_type_class_ref (driver);
      const FpIdEntry *entry;

      if (cls->type != FP_DEVICE_TYPE_USB)
        continue;

      for (entry = cls->id_table; entry->pid; entry++)
        {
          gint driver_score = 50;

          if (entry->pid != pid || entry->vid != vid)
            continue;

          if (cls->usb_discover)
            driver_score = cls->usb_discover (device);

          /* Is this driver better than the one we had? */
          if (driver_score <= found_score)
            continue;

          found_score = driver_score;
          found_driver = driver;
          found_entry = entry;
        }
    }

  if (found_driver == G_TYPE_NONE)
    {
      g_debug ("No driver found for USB device %04X:%04X", vid, pid);
      return;
    }

  priv->pending_devices++;
  g_async_initable_new_async (found_driver,
                              G_PRIORITY_LOW,
                              priv->cancellable,
                              async_device_init_done_cb,
                              self,
                              "fpi-usb-device", device,
                              "fpi-driver-data", found_entry->driver_data,
                              NULL);
}

static void
usb_device_removed_cb (FpContext *self, GUsbDevice *device, GUsbContext *usb_ctx)
{
  FpContextPrivate *priv = fp_context_get_instance_private (self);
  gint i;

  /* Do the lazy way and just look at each device. */
  for (i = 0; i < priv->devices->len; i++)
    {
      FpDevice *dev = g_ptr_array_index (priv->devices, i);
      FpDeviceClass *cls = FP_DEVICE_GET_CLASS (dev);

      if (cls->type != FP_DEVICE_TYPE_USB)
        continue;

      if (fpi_device_get_usb_device (dev) == device)
        fpi_device_remove (dev);
    }
}

static void
fp_context_finalize (GObject *object)
{
  FpContext *self = (FpContext *) object;
  FpContextPrivate *priv = fp_context_get_instance_private (self);

  g_clear_pointer (&priv->devices, g_ptr_array_unref);

  g_cancellable_cancel (priv->cancellable);
  g_clear_object (&priv->cancellable);
  g_clear_pointer (&priv->drivers, g_array_unref);

  g_slist_free_full (g_steal_pointer (&priv->sources), (GDestroyNotify) g_source_destroy);

  if (priv->usb_ctx)
    g_object_run_dispose (G_OBJECT (priv->usb_ctx));
  g_clear_object (&priv->usb_ctx);

  G_OBJECT_CLASS (fp_context_parent_class)->finalize (object);
}

static void
fp_context_class_init (FpContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = fp_context_finalize;

  /**
   * FpContext::device-added:
   * @context: the #FpContext instance that emitted the signal
   * @device: A #FpDevice
   *
   * This signal is emitted when a fingerprint reader is added.
   **/
  signals[DEVICE_ADDED_SIGNAL] = g_signal_new ("device-added",
                                               G_TYPE_FROM_CLASS (klass),
                                               G_SIGNAL_RUN_LAST,
                                               G_STRUCT_OFFSET (FpContextClass, device_added),
                                               NULL,
                                               NULL,
                                               g_cclosure_marshal_VOID__OBJECT,
                                               G_TYPE_NONE,
                                               1,
                                               FP_TYPE_DEVICE);

  /**
   * FpContext::device-removed:
   * @context: the #FpContext instance that emitted the signal
   * @device: A #FpDevice
   *
   * This signal is emitted when a fingerprint reader is removed.
   *
   * It is guaranteed that the device has been closed before this signal
   * is emitted. See the #FpDevice removed signal documentation for more
   * information.
   **/
  signals[DEVICE_REMOVED_SIGNAL] = g_signal_new ("device-removed",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 G_STRUCT_OFFSET (FpContextClass, device_removed),
                                                 NULL,
                                                 NULL,
                                                 g_cclosure_marshal_VOID__OBJECT,
                                                 G_TYPE_NONE,
                                                 1,
                                                 FP_TYPE_DEVICE);
}

static void
fp_context_init (FpContext *self)
{
  g_autoptr(GError) error = NULL;
  FpContextPrivate *priv = fp_context_get_instance_private (self);
  guint i;

  g_debug ("Initializing FpContext (libfprint version " LIBFPRINT_VERSION ")");

  priv->drivers = fpi_get_driver_types ();

  if (get_drivers_whitelist_env ())
    {
      for (i = 0; i < priv->drivers->len;)
        {
          GType driver = g_array_index (priv->drivers, GType, i);
          g_autoptr(FpDeviceClass) cls = g_type_class_ref (driver);

          if (!is_driver_allowed (cls->id))
            g_array_remove_index (priv->drivers, i);
          else
            ++i;
        }
    }

  priv->devices = g_ptr_array_new_with_free_func (g_object_unref);

  priv->cancellable = g_cancellable_new ();
  priv->usb_ctx = g_usb_context_new (&error);
  if (!priv->usb_ctx)
    {
      g_message ("Could not initialise USB Subsystem: %s", error->message);
    }
  else
    {
      g_usb_context_set_debug (priv->usb_ctx, G_LOG_LEVEL_INFO);
      g_signal_connect_object (priv->usb_ctx,
                               "device-added",
                               G_CALLBACK (usb_device_added_cb),
                               self,
                               G_CONNECT_SWAPPED);
      g_signal_connect_object (priv->usb_ctx,
                               "device-removed",
                               G_CALLBACK (usb_device_removed_cb),
                               self,
                               G_CONNECT_SWAPPED);
    }
}

/**
 * fp_context_new:
 *
 * Create a new #FpContext.
 *
 * Returns: (transfer full): a newly created #FpContext
 */
FpContext *
fp_context_new (void)
{
  return g_object_new (FP_TYPE_CONTEXT, NULL);
}

/**
 * fp_context_enumerate:
 * @context: a #FpContext
 *
 * Enumerate all devices. You should call this function exactly once
 * at startup. Please note that it iterates the mainloop until all
 * devices are enumerated.
 */
void
fp_context_enumerate (FpContext *context)
{
  FpContextPrivate *priv = fp_context_get_instance_private (context);
  gboolean dispatched;
  gint i;

  g_return_if_fail (FP_IS_CONTEXT (context));

  if (priv->enumerated)
    return;

  priv->enumerated = TRUE;

  /* USB devices are handled from callbacks */
  if (priv->usb_ctx)
    g_usb_context_enumerate (priv->usb_ctx);

  /* Handle Virtual devices based on environment variables */
  for (i = 0; i < priv->drivers->len; i++)
    {
      GType driver = g_array_index (priv->drivers, GType, i);
      g_autoptr(FpDeviceClass) cls = g_type_class_ref (driver);
      const FpIdEntry *entry;

      if (cls->type != FP_DEVICE_TYPE_VIRTUAL)
        continue;

      for (entry = cls->id_table; entry->pid; entry++)
        {
          const gchar *val;

          val = g_getenv (entry->virtual_envvar);
          if (!val || val[0] == '\0')
            continue;

          g_debug ("Found virtual environment device: %s, %s", entry->virtual_envvar, val);
          priv->pending_devices++;
          g_async_initable_new_async (driver,
                                      G_PRIORITY_LOW,
                                      priv->cancellable,
                                      async_device_init_done_cb,
                                      context,
                                      "fpi-environ", val,
                                      "fpi-driver-data", entry->driver_data,
                                      NULL);
          g_debug ("created");
        }
    }


#ifdef HAVE_UDEV
  {
    g_autoptr(GUdevClient) udev_client = g_udev_client_new (NULL);

    /* This uses a very simple algorithm to allocate devices to drivers and assumes that no two drivers will want the same device. Future improvements
     * could add a usb_discover style udev_discover that returns a score, however for internal devices the potential overlap should be very low between
     * separate drivers.
     */

    g_autoptr(GList) spidev_devices = g_udev_client_query_by_subsystem (udev_client, "spidev");
    g_autoptr(GList) hidraw_devices = g_udev_client_query_by_subsystem (udev_client, "hidraw");

    /* for each potential driver, try to match all requested resources. */
    for (i = 0; i < priv->drivers->len; i++)
      {
        GType driver = g_array_index (priv->drivers, GType, i);
        g_autoptr(FpDeviceClass) cls = g_type_class_ref (driver);
        const FpIdEntry *entry;

        if (cls->type != FP_DEVICE_TYPE_UDEV)
          continue;

        for (entry = cls->id_table; entry->udev_types; entry++)
          {
            GList *matched_spidev = NULL, *matched_hidraw = NULL;

            if (entry->udev_types & FPI_DEVICE_UDEV_SUBTYPE_SPIDEV)
              {
                for (matched_spidev = spidev_devices; matched_spidev; matched_spidev = matched_spidev->next)
                  {
                    const gchar * sysfs = g_udev_device_get_sysfs_path (matched_spidev->data);
                    if (!sysfs)
                      continue;
                    if (strstr (sysfs, entry->spi_acpi_id))
                      break;
                  }
                /* If match was not found exit */
                if (matched_spidev == NULL)
                  continue;
              }
            if (entry->udev_types & FPI_DEVICE_UDEV_SUBTYPE_HIDRAW)
              {
                for (matched_hidraw = hidraw_devices; matched_hidraw; matched_hidraw = matched_hidraw->next)
                  {
                    /* Find the parent HID node, and check the vid/pid from its HID_ID property */
                    g_autoptr(GUdevDevice) parent = g_udev_device_get_parent_with_subsystem (matched_hidraw->data, "hid", NULL);
                    const gchar * hid_id = g_udev_device_get_property (parent, "HID_ID");
                    guint32 vendor, product;

                    if (!parent || !hid_id)
                      continue;

                    if (sscanf (hid_id, "%*X:%X:%X", &vendor, &product) != 2)
                      continue;

                    if (vendor == entry->hid_id.vid && product == entry->hid_id.pid)
                      break;
                  }
                /* If match was not found exit */
                if (matched_hidraw == NULL)
                  continue;
              }
            priv->pending_devices++;
            g_async_initable_new_async (driver,
                                        G_PRIORITY_LOW,
                                        priv->cancellable,
                                        async_device_init_done_cb,
                                        context,
                                        "fpi-driver-data", entry->driver_data,
                                        "fpi-udev-data-spidev", (matched_spidev ? g_udev_device_get_device_file (matched_spidev->data) : NULL),
                                        "fpi-udev-data-hidraw", (matched_hidraw ? g_udev_device_get_device_file (matched_hidraw->data) : NULL),
                                        NULL);
            /* remove entries from list to avoid conflicts */
            if (matched_spidev)
              {
                g_object_unref (matched_spidev->data);
                spidev_devices = g_list_delete_link (spidev_devices, matched_spidev);
              }
            if (matched_hidraw)
              {
                g_object_unref (matched_hidraw->data);
                hidraw_devices = g_list_delete_link (hidraw_devices, matched_hidraw);
              }
          }
      }

    /* free all unused elemnts in both lists */
    g_list_foreach (spidev_devices, (GFunc) g_object_unref, NULL);
    g_list_foreach (hidraw_devices, (GFunc) g_object_unref, NULL);
  }
#endif

  /* Iterate until 1. we have no pending devices, and 2. the mainloop is idle
   * This takes care of processing hotplug events that happened during
   * enumeration.
   * This is important due to USB `persist` being turned off. At resume time,
   * devices will disappear and immediately re-appear. In this situation,
   * enumerate could first see the old state with a removed device resulting
   * in it to not be discovered.
   * As a hotplug event is seemingly emitted by the kernel immediately, we can
   * simply make sure to process all events before returning from enumerate.
   */
  dispatched = TRUE;
  while (priv->pending_devices || dispatched)
    dispatched = g_main_context_iteration (NULL, !!priv->pending_devices);
}

/**
 * fp_context_get_devices:
 * @context: a #FpContext
 *
 * Get all devices. fp_context_enumerate() will be called as needed.
 *
 * Returns: (transfer none) (element-type FpDevice): a new #GPtrArray of #FpDevice's.
 */
GPtrArray *
fp_context_get_devices (FpContext *context)
{
  FpContextPrivate *priv = fp_context_get_instance_private (context);

  g_return_val_if_fail (FP_IS_CONTEXT (context), NULL);

  fp_context_enumerate (context);

  return priv->devices;
}
