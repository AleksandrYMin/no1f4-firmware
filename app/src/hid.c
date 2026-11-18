/* BLE HID over GATT для S110 v8 — без библиотек SDK, только вызовы SoftDevice.
 *
 * Устройство: Consumer Control (мультимедийные клавиши), один входной отчёт
 * в 1 байт без Report ID: бит 0 — Volume Up (0xE9), бит 1 — Volume Down (0xEA).
 * Камеры Android (и iOS) делают снимок по Volume Up.
 *
 * Безопасность: HOGP требует шифрования. Сопряжение Just Works с бондингом;
 * ключи (наш LTK и IRK телефона) пока только в ОЗУ — после перезагрузки часов
 * телефон надо «забыть» и сопрячь заново. Подписки (CCCD) запоминаем при
 * разрыве и восстанавливаем при переподключении: Android после бондинга
 * их заново не включает.
 */
#include "hid.h"

#include <string.h>

#include "nrf.h"
#include "ble_gap.h"
#include "ble_gatts.h"
#include "ble_types.h"

#define UUID_HID_SERVICE       0x1812
#define UUID_BAS_SERVICE       0x180F
#define UUID_DIS_SERVICE       0x180A
#define UUID_PROTOCOL_MODE     0x2A4E
#define UUID_REPORT            0x2A4D
#define UUID_REPORT_MAP        0x2A4B
#define UUID_HID_INFO          0x2A4A
#define UUID_HID_CONTROL_POINT 0x2A4C
#define UUID_BATTERY_LEVEL     0x2A19
#define UUID_PNP_ID            0x2A50
#define UUID_REPORT_REFERENCE  0x2908

#define KEY_VOLUME_UP          0x01
#define SYS_ATTR_MAX           64

static const uint8_t m_report_map[] = {
    0x05, 0x0C,       /* Usage Page (Consumer) */
    0x09, 0x01,       /* Usage (Consumer Control) */
    0xA1, 0x01,       /* Collection (Application) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x01,       /*   Report Count (1) */
    0x09, 0xE9,       /*   Usage (Volume Up) */
    0x81, 0x02,       /*   Input (Data, Var, Abs) */
    0x09, 0xEA,       /*   Usage (Volume Down) */
    0x81, 0x02,       /*   Input (Data, Var, Abs) */
    0x95, 0x06,       /*   Report Count (6) — добивка до байта */
    0x81, 0x03,       /*   Input (Const) */
    0xC0,             /* End Collection */
};

/* bcdHID 1.11, страна 0, флаги: NormallyConnectable. */
static const uint8_t m_hid_info[] = { 0x11, 0x01, 0x00, 0x02 };
/* Report Reference: ID 0 (в карте нет Report ID), тип 1 — Input. */
static const uint8_t m_report_ref[] = { 0x00, 0x01 };
/* PnP ID: источник 2 (USB-IF), VID 0x1915 (Nordic), PID 0xEEF4, версия 0x0001. */
static const uint8_t m_pnp_id[] = { 0x02, 0x15, 0x19, 0xF4, 0xEE, 0x01, 0x00 };

static ble_gatts_char_handles_t m_report;
static ble_gatts_char_handles_t m_protocol_mode;
static ble_gatts_char_handles_t m_control_point;
static ble_gatts_char_handles_t m_battery;

static uint16_t            m_conn = BLE_CONN_HANDLE_INVALID;
static bool                m_encrypted;
static bool                m_release_pending;  /* «отпустить» не влезло в буферы — шлём по TX_COMPLETE */

/* Бонд в ОЗУ (один телефон). */
static bool                m_bonded;
static ble_gap_enc_key_t   m_own_enc;          /* наш LTK и EDIV/RAND, которые раздали телефону */
static ble_gap_id_key_t    m_peer_id;          /* IRK и адрес телефона */
static ble_gap_sec_keyset_t m_keyset;
static uint8_t             m_sys_attr[SYS_ATTR_MAX];
static uint16_t            m_sys_attr_len;


static void perm_enc(ble_gap_conn_sec_mode_t * p_mode)
{
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(p_mode);
}

/* Характеристика с 16-битным UUID. read_enc/write_enc — требовать шифрование. */
static uint32_t char_add(uint16_t service, uint16_t uuid, ble_gatt_char_props_t props,
                         bool read_enc, bool write_enc, const uint8_t * p_init,
                         uint16_t init_len, uint16_t max_len, bool vlen,
                         ble_gatts_char_handles_t * p_handles)
{
    ble_gatts_char_md_t char_md;
    ble_gatts_attr_md_t cccd_md;
    ble_gatts_attr_md_t attr_md;
    ble_gatts_attr_t    attr;
    ble_uuid_t          char_uuid = { .uuid = uuid, .type = BLE_UUID_TYPE_BLE };

    memset(&char_md, 0, sizeof(char_md));
    memset(&cccd_md, 0, sizeof(cccd_md));
    memset(&attr_md, 0, sizeof(attr_md));
    memset(&attr, 0, sizeof(attr));

    char_md.char_props = props;
    if (props.notify)
    {
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
        perm_enc(&cccd_md.write_perm);
        cccd_md.vloc = BLE_GATTS_VLOC_STACK;
        char_md.p_cccd_md = &cccd_md;
    }

    if (read_enc)
    {
        perm_enc(&attr_md.read_perm);
    }
    else
    {
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    }
    if (props.write || props.write_wo_resp)
    {
        if (write_enc)
        {
            perm_enc(&attr_md.write_perm);
        }
        else
        {
            BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
        }
    }
    else
    {
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&attr_md.write_perm);
    }
    attr_md.vloc = BLE_GATTS_VLOC_STACK;
    attr_md.vlen = vlen ? 1 : 0;

    attr.p_uuid    = &char_uuid;
    attr.p_attr_md = &attr_md;
    attr.p_value   = (uint8_t *)p_init;
    attr.init_len  = init_len;
    attr.max_len   = max_len;

    return sd_ble_gatts_characteristic_add(service, &char_md, &attr, p_handles);
}

static uint32_t service_add(uint16_t uuid, uint16_t * p_service)
{
    ble_uuid_t u = { .uuid = uuid, .type = BLE_UUID_TYPE_BLE };

    return sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &u, p_service);
}

#define TRY(call) do { uint32_t err_ = (call); if (err_ != NRF_SUCCESS) return err_; } while (0)

uint32_t hid_init(void)
{
    uint16_t              service;
    ble_gatt_char_props_t props;
    uint8_t               zero = 0;
    uint8_t               report_mode = 0x01;
    uint8_t               battery = 100;

    /* HID */
    TRY(service_add(UUID_HID_SERVICE, &service));

    memset(&props, 0, sizeof(props));
    props.read = 1;
    props.write_wo_resp = 1;
    TRY(char_add(service, UUID_PROTOCOL_MODE, props, true, true, &report_mode, 1, 1, false,
                 &m_protocol_mode));

    memset(&props, 0, sizeof(props));
    props.read = 1;
    props.notify = 1;
    TRY(char_add(service, UUID_REPORT, props, true, true, &zero, 1, 1, false, &m_report));
    {
        ble_gatts_attr_md_t md;
        ble_gatts_attr_t    attr;
        ble_uuid_t          u = { .uuid = UUID_REPORT_REFERENCE, .type = BLE_UUID_TYPE_BLE };
        uint16_t            handle;

        memset(&md, 0, sizeof(md));
        memset(&attr, 0, sizeof(attr));
        perm_enc(&md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&md.write_perm);
        md.vloc        = BLE_GATTS_VLOC_STACK;
        attr.p_uuid    = &u;
        attr.p_attr_md = &md;
        attr.p_value   = (uint8_t *)m_report_ref;
        attr.init_len  = sizeof(m_report_ref);
        attr.max_len   = sizeof(m_report_ref);
        TRY(sd_ble_gatts_descriptor_add(m_report.value_handle, &attr, &handle));
    }

    memset(&props, 0, sizeof(props));
    props.read = 1;
    {
        ble_gatts_char_handles_t h;
        TRY(char_add(service, UUID_REPORT_MAP, props, true, false, m_report_map,
                     sizeof(m_report_map), sizeof(m_report_map), false, &h));
        TRY(char_add(service, UUID_HID_INFO, props, true, false, m_hid_info,
                     sizeof(m_hid_info), sizeof(m_hid_info), false, &h));
    }

    memset(&props, 0, sizeof(props));
    props.write_wo_resp = 1;
    TRY(char_add(service, UUID_HID_CONTROL_POINT, props, true, true, &zero, 1, 1, false,
                 &m_control_point));

    /* Battery */
    TRY(service_add(UUID_BAS_SERVICE, &service));
    memset(&props, 0, sizeof(props));
    props.read = 1;
    TRY(char_add(service, UUID_BATTERY_LEVEL, props, false, false, &battery, 1, 1, false,
                 &m_battery));

    /* Device Information: PnP ID */
    TRY(service_add(UUID_DIS_SERVICE, &service));
    {
        ble_gatts_char_handles_t h;
        TRY(char_add(service, UUID_PNP_ID, props, false, false, m_pnp_id, sizeof(m_pnp_id),
                     sizeof(m_pnp_id), false, &h));
    }

    m_keyset.keys_periph.p_enc_key  = &m_own_enc;
    m_keyset.keys_central.p_id_key  = &m_peer_id;
    return NRF_SUCCESS;
}

void hid_battery_set(uint8_t percent)
{
    ble_gatts_value_t v = { .len = 1, .offset = 0, .p_value = &percent };

    if (percent > 100)
    {
        percent = 100;
    }
    sd_ble_gatts_value_set(BLE_CONN_HANDLE_INVALID, m_battery.value_handle, &v);
}

static uint32_t report_send(uint8_t keys)
{
    uint16_t               len = 1;
    ble_gatts_hvx_params_t hvx = {
        .handle = m_report.value_handle,
        .type   = BLE_GATT_HVX_NOTIFICATION,
        .p_len  = &len,
        .p_data = &keys,
    };

    return sd_ble_gatts_hvx(m_conn, &hvx);
}

static bool notify_enabled(void)
{
    uint8_t           cccd[2] = { 0 };
    ble_gatts_value_t v = { .len = 2, .offset = 0, .p_value = cccd };

    if (sd_ble_gatts_value_get(m_conn, m_report.cccd_handle, &v) != NRF_SUCCESS)
    {
        return false;
    }
    return (cccd[0] & BLE_GATT_HVX_NOTIFICATION) != 0;
}

bool hid_ready(void)
{
    return m_conn != BLE_CONN_HANDLE_INVALID && m_encrypted && notify_enabled();
}

bool hid_volume_up_click(void)
{
    if (!hid_ready())
    {
        return false;
    }
    if (report_send(KEY_VOLUME_UP) != NRF_SUCCESS)
    {
        return false;
    }
    m_release_pending = report_send(0) != NRF_SUCCESS;
    return true;
}

void hid_on_ble_evt(const ble_evt_t * p_evt)
{
    switch (p_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            m_conn = p_evt->evt.gap_evt.conn_handle;
            m_encrypted = false;
            m_release_pending = false;
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            if (m_bonded)
            {
                /* Запомнить подписки телефона до следующего подключения. */
                uint16_t len = sizeof(m_sys_attr);
                m_sys_attr_len = sd_ble_gatts_sys_attr_get(m_conn, m_sys_attr, &len, 0)
                                 == NRF_SUCCESS ? len : 0;
            }
            m_conn = BLE_CONN_HANDLE_INVALID;
            m_encrypted = false;
            break;

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
        {
            ble_gap_sec_params_t sp;

            memset(&sp, 0, sizeof(sp));
            sp.bond               = 1;
            sp.mitm               = 0;
            sp.io_caps            = BLE_GAP_IO_CAPS_NONE;
            sp.oob                = 0;
            sp.min_key_size       = 7;
            sp.max_key_size       = 16;
            sp.kdist_periph.enc   = 1;
            sp.kdist_central.id   = 1;
            /* Новое сопряжение заменяет старый бонд. */
            m_bonded = false;
            m_sys_attr_len = 0;
            sd_ble_gap_sec_params_reply(m_conn, BLE_GAP_SEC_STATUS_SUCCESS, &sp, &m_keyset);
            break;
        }

        case BLE_GAP_EVT_AUTH_STATUS:
            if (p_evt->evt.gap_evt.params.auth_status.auth_status == BLE_GAP_SEC_STATUS_SUCCESS
                && p_evt->evt.gap_evt.params.auth_status.bonded)
            {
                m_bonded = true;
            }
            break;

        case BLE_GAP_EVT_SEC_INFO_REQUEST:
        {
            const ble_gap_master_id_t * p_id = &p_evt->evt.gap_evt.params.sec_info_request.master_id;

            if (m_bonded && p_id->ediv == m_own_enc.master_id.ediv
                && memcmp(p_id->rand, m_own_enc.master_id.rand, BLE_GAP_SEC_RAND_LEN) == 0)
            {
                sd_ble_gap_sec_info_reply(m_conn, &m_own_enc.enc_info, NULL, NULL);
            }
            else
            {
                /* Ключа нет (часы перезагружались) — телефону придётся сопрягаться заново. */
                sd_ble_gap_sec_info_reply(m_conn, NULL, NULL, NULL);
            }
            break;
        }

        case BLE_GAP_EVT_CONN_SEC_UPDATE:
            m_encrypted = p_evt->evt.gap_evt.params.conn_sec_update.conn_sec.sec_mode.lv >= 2;
            break;

        case BLE_GATTS_EVT_SYS_ATTR_MISSING:
            sd_ble_gatts_sys_attr_set(m_conn, m_sys_attr_len ? m_sys_attr : NULL, m_sys_attr_len, 0);
            break;

        case BLE_EVT_TX_COMPLETE:
            if (m_release_pending && m_conn != BLE_CONN_HANDLE_INVALID)
            {
                m_release_pending = report_send(0) != NRF_SUCCESS;
            }
            break;

        default:
            break;
    }
}
