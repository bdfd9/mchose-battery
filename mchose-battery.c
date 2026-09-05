#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#define DRIVER_NAME "mchose-battery"

/// How often to poll the device
#define POLL_INTERVAL_MS (15 * 1000)
/// 2s seem to be always working
#define RESPONSE_TIMEOUT_MS 2000

#define REPORT_SIZE 21
#define REPORT_ID 0x11
#define REQUEST_REFRESH_CMD 0x6

/*******************************************************************************/
/* Structs                                                                     */
/*******************************************************************************/

typedef struct mouse_battery_data {
    struct hid_device* hdev;

    struct power_supply* battery;
    struct power_supply_desc battery_desc;
    char battery_name[255];

    struct delayed_work poll_work;

    spinlock_t lock;

    int ps_status;
    u8 battery_level;
    bool is_charging;
} mouse_battery_data_t;

int mbat_query_battery(mouse_battery_data_t* data);

/*******************************************************************************/
/* power_supply interface                                                      */
/*******************************************************************************/

/*******************************************************************************/
/* Sysfs                                                                       */
/*******************************************************************************/

/*
 * Writing anything to this file triggers an immediate battery query.
 * e.g.  echo 1 | sudo tee /sys/class/power_supply/{NAME}/refresh
 */
static ssize_t refresh_store(
    struct device* dev, //
    struct device_attribute* attr,
    const char* buf,
    size_t count
) {
    struct power_supply* const psy = dev_get_drvdata(dev);
    mouse_battery_data_t* const data = power_supply_get_drvdata(psy);

    hid_info(data->hdev, "refreshing `%s`\n", data->hdev->name);

    const int ret = mbat_query_battery(data);
    if (ret) {
        return ret;
    }

    power_supply_changed(data->battery);

    return count;
}
static DEVICE_ATTR_WO(refresh);

static struct attribute* mbat_attrs[] = {
    &dev_attr_refresh.attr,
    NULL,
};
static const struct attribute_group mbat_attr_group = {
    .attrs = mbat_attrs,
};
static const struct attribute_group* mbat_attr_groups[] = {
    &mbat_attr_group,
    NULL,
};

static int mbat_ps_get_property(
    struct power_supply* psy,
    enum power_supply_property psp,
    union power_supply_propval* val
) {
    mouse_battery_data_t* const data = power_supply_get_drvdata(psy);
    int ret = 0;

    unsigned long flags = 0;
    spin_lock_irqsave(&data->lock, flags);

    switch (psp) {
        case POWER_SUPPLY_PROP_STATUS: {
            val->intval = data->ps_status;
            break;
        }
        case POWER_SUPPLY_PROP_CAPACITY: {
            val->intval = data->battery_level;
            break;
        }
        case POWER_SUPPLY_PROP_CAPACITY_LEVEL: {
            if (data->battery_level > 80)
                val->intval = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
            else if (data->battery_level > 40)
                val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
            else if (data->battery_level > 10)
                val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
            else
                val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
            break;
        }
        case POWER_SUPPLY_PROP_SCOPE: {
            val->intval = POWER_SUPPLY_SCOPE_DEVICE;
            break;
        }
        case POWER_SUPPLY_PROP_MODEL_NAME: {
            /* val->strval must point to a stable string */
            val->strval = data->hdev->name;
            break;
        }
        case POWER_SUPPLY_PROP_MANUFACTURER: {
            val->strval = "MCHOSE";
            break;
        }
        case POWER_SUPPLY_PROP_SERIAL_NUMBER: {
            val->strval = data->hdev->uniq;
            break;
        }
        default:
            ret = -EINVAL;
            break;
    }

    spin_unlock_irqrestore(&data->lock, flags);
    return ret;
}

static enum power_supply_property mbat_ps_props[] = {
    POWER_SUPPLY_PROP_STATUS,         //
    POWER_SUPPLY_PROP_CAPACITY,       //
    POWER_SUPPLY_PROP_CAPACITY_LEVEL, //
    POWER_SUPPLY_PROP_SCOPE,          //
    POWER_SUPPLY_PROP_MODEL_NAME,     //
    POWER_SUPPLY_PROP_MANUFACTURER,   //
    POWER_SUPPLY_PROP_SERIAL_NUMBER,  //
};

/*******************************************************************************/
/* Pooling                                                                     */
/*******************************************************************************/

int mbat_query_battery(mouse_battery_data_t* data) {
    struct hid_device* const hdev = data->hdev;

    unsigned char* buffer = kzalloc(REPORT_SIZE, GFP_KERNEL);
    if (!buffer) {
        return -ENOMEM;
    }
    memset(buffer, 0x0, REPORT_SIZE);

    // Step 1: Trigger a refresh
    buffer[0] = REPORT_ID;
    buffer[1] = REQUEST_REFRESH_CMD;

    // #NOTE: They are using XOR as an obfuscation method.
    for (int i = 1; i < REPORT_SIZE; ++i) {
        buffer[i] ^= 0xff;
    }

    int ret = 0;
    ret = hid_hw_raw_request(
        hdev, //
        REPORT_ID,
        buffer,
        REPORT_SIZE,
        HID_FEATURE_REPORT,
        HID_REQ_SET_REPORT
    );

    if (ret < 0) {
        hid_err(hdev, "SET_FEATURE report 0x11 failed: %d\n", ret);
        goto out;
    }

    // Step 2: wait a bit for the refresh
    msleep(RESPONSE_TIMEOUT_MS);

    // Step 3: read the new snapshot
    memset(buffer, 0x0, REPORT_SIZE);
    buffer[0] = REPORT_ID;

    ret = hid_hw_raw_request(
        hdev, //
        REPORT_ID,
        buffer,
        REPORT_SIZE,
        HID_FEATURE_REPORT,
        HID_REQ_GET_REPORT
    );
    if (ret < 0) {
        hid_err(hdev, "GET_FEATURE report 0x11 failed: %d\n", ret);
        goto out;
    }

    // Step 4: extract values
    //
    // #NOTE: They are using XOR as an obfuscation method.
    for (int i = 1; i < REPORT_SIZE; ++i) {
        buffer[i] ^= 0xff;
    }

    unsigned char* const payload = buffer + 2;

    size_t offset = 0;
    uint16_t vid;
    uint16_t pid;
    uint32_t fw;
    uint8_t flags;
    uint8_t battery_level;
    uint8_t charge_status;

    memcpy(&vid, payload + offset, sizeof(vid));
    offset += 2;

    memcpy(&pid, payload + offset, sizeof(pid));
    offset += 2;

    memcpy(&fw, payload + offset, sizeof(fw));
    offset += 4;

    flags = *(payload + offset);
    offset += 1;

    battery_level = *(payload + offset);
    offset += 1;

    charge_status = *(payload + offset);
    offset += 1;

    // #NOTE: These are correct and have been left as documentation.
    // const uint8_t connect_mode = flags & 0x7;
    // const uint8_t in_reserved = (flags >> 4) & 0xf;
    const uint8_t connect_status = (flags >> 3) & 0x1;

    unsigned long lock_flags;
    spin_lock_irqsave(&data->lock, lock_flags);

    if (connect_status == 1 && (battery_level <= 100)) {
        // The mouse is connected.
        // {'command': 6, 'vid': 14391, 'pid': 16409, 'fwVersion': 67251205, 'connectMode': 1, 'connectStatus': 1, 'inReserved': 0, 'batteryLevel': 47, 'chargeStatus': 0}
        int ps_status = POWER_SUPPLY_STATUS_UNKNOWN;
        if (battery_level == 100) {
            // If the battery is fully charged (i.e. 100%),
            // we can consider it full, even if the device
            // is still reporting that it is in 'charge' mode.
            ps_status = POWER_SUPPLY_STATUS_FULL;
        } else if (charge_status == 1) {
            ps_status = POWER_SUPPLY_STATUS_CHARGING;
        } else {
            ps_status = POWER_SUPPLY_STATUS_DISCHARGING;
        }

        data->ps_status = ps_status;
        data->battery_level = battery_level;
        data->is_charging = charge_status;
    } else {
        if (battery_level > 100) {
            hid_warn(
                data->hdev, //
                "[%s]: battery_level should be <= 100 (is %d)\n",
                data->hdev->name,
                battery_level
            );
        }

        // Here the mouse can be:
        // - powered off
        // - sleeping after inactivity
        // - waking up from the sleep
        //
        // The values seem to be all over the place.
        // Everything can be set to 0 when the mouse is powered off.
        // The connect mode can be set to 6 when waking up or 7 when powered off.
        //
        // 1. Powered off
        // - {'command': 6, 'vid': 0, 'pid': 0, 'fwVersion': 0, 'connectMode': 0, 'connectStatus': 0, 'inReserved': 0, 'batteryLevel': 0, 'chargeStatus': 0}
        // - {'command': 249, 'vid': 65535, 'pid': 65535, 'fwVersion': 4294967295, 'connectMode': 7, 'connectStatus': 1, 'inReserved': 15, 'batteryLevel': 255, 'chargeStatus': 255}
        // 2. Sleeping
        // - {'command': 249, 'vid': 51144, 'pid': 49126, 'fwVersion': 4227716090, 'connectMode': 6, 'connectStatus': 0, 'inReserved': 15, 'batteryLevel': 207, 'chargeStatus': 255}
        //
        // Therefore, do not update anything and set the power state to 'unknown'.
        hid_info(
            data->hdev, //
            "[%s]: the mouse is either powered off or in sleep mode...\n",
            data->hdev->name
        );

        data->ps_status = POWER_SUPPLY_STATUS_UNKNOWN;
    }

    spin_unlock_irqrestore(&data->lock, lock_flags);

    hid_info(
        data->hdev,
        "battery: %d%% [%s]\n",
        data->battery_level,
        (data->ps_status == POWER_SUPPLY_STATUS_CHARGING)      ? "charging"
        : (data->ps_status == POWER_SUPPLY_STATUS_FULL)        ? "full"
        : (data->ps_status == POWER_SUPPLY_STATUS_DISCHARGING) ? "discharging"
                                                               : "unknown"
    );

    ret = 0;

out:
    kfree(buffer);
    return ret;
}

static void mbat_poll_work(struct work_struct* work) {
    mouse_battery_data_t* data //
        = container_of(work, mouse_battery_data_t, poll_work.work);
    int ret = -1;

    ret = mbat_query_battery(data);
    if (ret) {
        hid_warn(data->hdev, "battery read failed: %d\n", ret);
    }

    // Notify UPower / kernel regardless of success.
    power_supply_changed(data->battery);

    if (ret) {
        hid_warn(data->hdev, "poll: battery query failed (%d)\n", ret);
    }

    schedule_delayed_work(&data->poll_work, POLL_INTERVAL_MS);
}

/*******************************************************************************/
/* probe & remove                                                              */
/*******************************************************************************/

static void sanitize_name(char* const s) {
    for (size_t i = 0; s[i] != '\0'; i++) {
        const char c = s[i];

        if (isupper(c)) {
            s[i] = tolower(c);
        }

        if (!(isalnum(c) || (c == '-') || (c == '_'))) {
            s[i] = '-';
        }
    }
}

static int mbat_register_power_supply(
    struct hid_device* hdev, //
    mouse_battery_data_t* data
) {
    struct power_supply_config psy_cfg = {};
    psy_cfg.drv_data = data;
    psy_cfg.attr_grp = mbat_attr_groups;

    data->battery_desc = (struct power_supply_desc) {
        .name = data->battery_name,
        .type = POWER_SUPPLY_TYPE_BATTERY,
        .properties = mbat_ps_props,
        .num_properties = ARRAY_SIZE(mbat_ps_props),
        .get_property = mbat_ps_get_property,
    };

    data->battery = devm_power_supply_register(&hdev->dev, &data->battery_desc, &psy_cfg);
    if (IS_ERR(data->battery)) {
        int ret = PTR_ERR(data->battery);
        hid_err(hdev, "devm_power_supply_register failed: %d\n", ret);
        return ret;
    }

    return 0;
}

static int mbat_probe_battery(struct hid_device* hdev) {
    int ret = -1;

    ret = hid_hw_power(hdev, PM_HINT_FULLON);
    if (ret < 0) {
        hid_err(hdev, "hid_hw_power(FULLON) failed: %d\n", ret);
        return ret;
    }

    ret = hid_hw_open(hdev);
    if (ret) {
        hid_err(hdev, "hid_hw_open failed: %d\n", ret);
        goto err_power;
    }

    mouse_battery_data_t* data //
        = devm_kzalloc(&hdev->dev, sizeof(mouse_battery_data_t), GFP_KERNEL);
    if (!data) {
        ret = -ENOMEM;
        goto err_close;
    }

    // #NOTE: mchose hardcoded serial numbers to 0123456789, so it won't be fully unique...
    memset(data->battery_name, 0x0, sizeof(data->battery_name));
    snprintf(
        data->battery_name, //
        sizeof(data->battery_name),
        "%s-%s",
        hdev->name,
        hdev->uniq
    );
    sanitize_name(data->battery_name);

    data->hdev = hdev;
    data->is_charging = false;
    data->ps_status = POWER_SUPPLY_STATUS_UNKNOWN;
    data->battery_level = 0;

    ret = mbat_register_power_supply(hdev, data);
    if (ret) {
        goto err_close;
    }

    hid_set_drvdata(hdev, data);
    INIT_DELAYED_WORK(&data->poll_work, mbat_poll_work);
    schedule_delayed_work(&data->poll_work, msecs_to_jiffies(2000));

    hid_info(hdev, "registered: `%s` (poll every %u ms)\n", hdev->name, POLL_INTERVAL_MS);
    return 0;

err_close:
    hid_hw_close(hdev);
err_power:
    hid_hw_power(hdev, PM_HINT_NORMAL);
    return ret;
}

/// Seems to be working on `input2`
static bool mbat_is_battery_iface(struct hid_device* hdev) {
    return strstr(hdev->phys, "input2") != NULL;
}

static int mbat_probe(struct hid_device* hdev, const struct hid_device_id* id) {
    int ret = -1;

    ret = hid_parse(hdev);
    if (ret) {
        hid_err(hdev, "hid_parse failed: %d\n", ret);
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret) {
        hid_err(hdev, "hid_hw_start failed: %d\n", ret);
        return ret;
    }

    if (!mbat_is_battery_iface(hdev)) {
        hid_dbg(hdev, "non-battery interface, acting as hid-generic\n");
        return 0;
    }

    dev_info(&hdev->dev, "uniq: %s\n", hdev->uniq);

    ret = mbat_probe_battery(hdev);
    if (ret) {
        hid_hw_stop(hdev);
        return ret;
    }

    return 0;
}

static void mbat_remove(struct hid_device* hdev) {
    struct mouse_battery_data* data = hid_get_drvdata(hdev);

    if (data != NULL) {
        cancel_delayed_work_sync(&data->poll_work);

        hid_hw_close(hdev);
        hid_hw_power(hdev, PM_HINT_NORMAL);
    }

    hid_hw_stop(hdev);
    hid_info(hdev, "removed: %s\n", hdev->name);
}

/*******************************************************************************/
/* Device table                                                                */
/*******************************************************************************/

static const struct hid_device_id mbat_devices[] = {
    // A7 V2 Ultra, dongle
    { HID_USB_DEVICE(0x3837, 0x100b) },
    // L7 Ultra, dongle
    { HID_USB_DEVICE(0x5253, 0x1020) },
    {},
};
MODULE_DEVICE_TABLE(hid, mbat_devices);

static struct hid_driver mbat_driver = {
    .name = DRIVER_NAME,
    .id_table = mbat_devices,
    .probe = mbat_probe,
    .remove = mbat_remove,
};

module_hid_driver(mbat_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("bdfd9");
MODULE_DESCRIPTION("MCHOSE battery driver");
MODULE_VERSION("1.0.0");
