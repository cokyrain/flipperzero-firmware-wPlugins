#include "../metroflip_i.h"

#include <bit_lib.h>
#include <flipper_application.h>
#include <furi.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_sync.h>
#include <nfc/protocols/mf_classic/mf_classic.h>
#include <nfc/protocols/mf_classic/mf_classic_poller.h>
#include <string.h>
#include <dolphin/dolphin.h>
#include <furi_hal.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_listener.h>
#include <storage/storage.h>

#define MAX_TRIPS           10
#define TAG                 "Metroflip:Scene:Smartrider"
#define MAX_BLOCKS          64
#define MAX_DATE_ITERATIONS 366

uint8_t sector_num = 0;

static const uint8_t STANDARD_KEYS[3][6] = {
    {0x20, 0x31, 0xD1, 0xE5, 0x7A, 0x3B},
    {0x4C, 0xA6, 0x02, 0x9F, 0x94, 0x73},
    {0x19, 0x19, 0x53, 0x98, 0xE3, 0x2F}};

void uid_to_string(const uint8_t* uid, size_t uid_len, char* uid_str, size_t max_len) {
    size_t pos = 0;

    for(size_t i = 0; i < uid_len && pos + 2 < max_len; ++i) {
        pos += snprintf(&uid_str[pos], max_len - pos, "%02X", uid[i]);
    }

    uid_str[pos] = '\0'; // Null-terminate the string
}

typedef struct {
    uint32_t timestamp;
    uint16_t cost;
    uint16_t transaction_number;
    uint16_t journey_number;
    char route[5];
    uint8_t tap_on : 1;
    uint8_t block;
} __attribute__((packed)) TripData;

typedef struct {
    uint32_t balance;
    uint16_t issued_days;
    uint16_t expiry_days;
    uint16_t purchase_cost;
    uint16_t auto_load_threshold;
    uint16_t auto_load_value;
    char card_serial_number[11];
    uint8_t token;
    TripData trips[MAX_TRIPS];
    uint8_t trip_count;
} __attribute__((packed)) SmartRiderData;

static const char* const CONCESSION_TYPES[] = {
    "Pre-issue",
    "Standard Fare",
    "Student",
    NULL,
    "Tertiary",
    NULL,
    "Seniors",
    "Health Care",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    "PTA Staff",
    "Pensioner",
    "Free Travel"};

static inline const char* get_concession_type(uint8_t token) {
    return (token <= 0x10) ? CONCESSION_TYPES[token] : "Unknown";
}

static inline bool
    parse_trip_data(const MfClassicBlock* block_data, TripData* trip, uint8_t block_number) {
    trip->timestamp = bit_lib_bytes_to_num_le(block_data->data + 3, 4);
    trip->tap_on = (block_data->data[7] & 0x10) == 0x10;
    memcpy(trip->route, block_data->data + 8, 4);
    trip->route[4] = '\0';
    trip->cost = bit_lib_bytes_to_num_le(block_data->data + 13, 2);
    trip->transaction_number = bit_lib_bytes_to_num_le(block_data->data, 2);
    trip->journey_number = bit_lib_bytes_to_num_le(block_data->data + 2, 2);
    trip->block = block_number;
    return true;
}

static bool is_leap_year(uint16_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static void calculate_date(uint32_t timestamp, char* date_str, size_t date_str_size) {
    uint32_t seconds_since_2000 = timestamp;
    uint32_t days_since_2000 = seconds_since_2000 / 86400;
    uint16_t year = 2000;
    uint8_t month = 1;
    uint16_t day = 1;

    static const uint16_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    while(days_since_2000 >= (is_leap_year(year) ? 366 : 365)) {
        days_since_2000 -= (is_leap_year(year) ? 366 : 365);
        year++;
    }

    for(month = 0; month < 12; month++) {
        uint16_t dim = days_in_month[month];
        if(month == 1 && is_leap_year(year)) {
            dim++;
        }
        if(days_since_2000 < dim) {
            break;
        }
        days_since_2000 -= dim;
    }

    day = days_since_2000 + 1;
    month++; // Adjust month to 1-based

    if(date_str_size > 0) {
        size_t written = 0;
        written += snprintf(date_str + written, date_str_size - written, "%02u", day);
        if(written < date_str_size - 1) {
            written += snprintf(date_str + written, date_str_size - written, "/");
        }
        if(written < date_str_size - 1) {
            written += snprintf(date_str + written, date_str_size - written, "%02u", month);
        }
        if(written < date_str_size - 1) {
            written += snprintf(date_str + written, date_str_size - written, "/");
        }
        if(written < date_str_size - 1) {
            snprintf(date_str + written, date_str_size - written, "%02u", year % 100);
        }
    } else {
        // If the buffer size is 0, do nothing
    }
}

static bool smartrider_parse(const NfcDevice* device, FuriString* parsed_data) {
    furi_assert(device);
    furi_assert(parsed_data);
    const MfClassicData* data = nfc_device_get_data(device, NfcProtocolMfClassic);
    SmartRiderData sr_data = {0};

    if(data->type != MfClassicType1k) {
        FURI_LOG_E(TAG, "Invalid card type");
        return false;
    }

    const MfClassicSectorTrailer* sec_tr = mf_classic_get_sector_trailer_by_sector(data, 0);
    if(!sec_tr || memcmp(sec_tr->key_a.data, STANDARD_KEYS[0], 6) != 0) {
        FURI_LOG_E(TAG, "Key verification failed for sector 0");
        return false;
    }

    static const uint8_t required_blocks[] = {14, 4, 5, 1, 52, 50, 0};
    for(size_t i = 0; i < COUNT_OF(required_blocks); i++) {
        if(required_blocks[i] >= MAX_BLOCKS ||
           !mf_classic_is_block_read(data, required_blocks[i])) {
            FURI_LOG_E(TAG, "Required block %d is not read or out of range", required_blocks[i]);
            return false;
        }
    }

    sr_data.balance = bit_lib_bytes_to_num_le(data->block[14].data + 7, 2);
    sr_data.issued_days = bit_lib_bytes_to_num_le(data->block[4].data + 16, 2);
    sr_data.expiry_days = bit_lib_bytes_to_num_le(data->block[4].data + 18, 2);
    sr_data.auto_load_threshold = bit_lib_bytes_to_num_le(data->block[4].data + 20, 2);
    sr_data.auto_load_value = bit_lib_bytes_to_num_le(data->block[4].data + 22, 2);
    sr_data.token = data->block[5].data[8];
    sr_data.purchase_cost = bit_lib_bytes_to_num_le(data->block[0].data + 14, 2);

    snprintf(
        sr_data.card_serial_number,
        sizeof(sr_data.card_serial_number),
        "%02X%02X%02X%02X%02X",
        data->block[1].data[6],
        data->block[1].data[7],
        data->block[1].data[8],
        data->block[1].data[9],
        data->block[1].data[10]);

    for(uint8_t block_number = 40; block_number <= 52 && sr_data.trip_count < MAX_TRIPS;
        block_number++) {
        if((block_number != 43 && block_number != 47 && block_number != 51) &&
           mf_classic_is_block_read(data, block_number) &&
           parse_trip_data(
               &data->block[block_number], &sr_data.trips[sr_data.trip_count], block_number)) {
            sr_data.trip_count++;
        }
    }

    // Sort trips by timestamp (descending order)
    for(uint8_t i = 0; i < sr_data.trip_count - 1; i++) {
        for(uint8_t j = 0; j < sr_data.trip_count - i - 1; j++) {
            if(sr_data.trips[j].timestamp < sr_data.trips[j + 1].timestamp) {
                TripData temp = sr_data.trips[j];
                sr_data.trips[j] = sr_data.trips[j + 1];
                sr_data.trips[j + 1] = temp;
            }
        }
    }

    furi_string_printf(
        parsed_data,
        "\e#SmartRider\nBalance: $%lu.%02lu\nConcession: %s\nSerial: %s%s\n"
        "Total Cost: $%u.%02u\nAuto-Load: $%u.%02u/$%u.%02u\n\e#Tag On/Off History\n",
        sr_data.balance / 100,
        sr_data.balance % 100,
        get_concession_type(sr_data.token),
        memcmp(sr_data.card_serial_number, "00", 2) == 0 ? "SR0" : "",
        memcmp(sr_data.card_serial_number, "00", 2) == 0 ? sr_data.card_serial_number + 2 :
                                                           sr_data.card_serial_number,
        sr_data.purchase_cost / 100,
        sr_data.purchase_cost % 100,
        sr_data.auto_load_threshold / 100,
        sr_data.auto_load_threshold % 100,
        sr_data.auto_load_value / 100,
        sr_data.auto_load_value % 100);

    for(uint8_t i = 0; i < sr_data.trip_count; i++) {
        char date_str[9];
        calculate_date(sr_data.trips[i].timestamp, date_str, sizeof(date_str));

        uint32_t cost = sr_data.trips[i].cost;
        if(cost > 0) {
            furi_string_cat_printf(
                parsed_data,
                "%s %c $%lu.%02lu %s\n",
                date_str,
                sr_data.trips[i].tap_on ? '+' : '-',
                cost / 100,
                cost % 100,
                sr_data.trips[i].route);
        } else {
            furi_string_cat_printf(
                parsed_data,
                "%s %c %s\n",
                date_str,
                sr_data.trips[i].tap_on ? '+' : '-',
                sr_data.trips[i].route);
        }
    }

    return true;
}

// made with love by jay candel <3

void handle_keyfile_case(
    Metroflip* app,
    const char* message_title,
    const char* log_message,
    FuriString* parsed_data) {
    FURI_LOG_I(TAG, log_message);
    dolphin_deed(DolphinDeedNfcReadSuccess);
    furi_string_reset(parsed_data);

    furi_string_printf(
        parsed_data,
        "\e#%s\n\n"
        "To read a SmartRider, \nyou need to read \nit in NFC "
        "app on \nthe flipper, and it\nneeds to show \n32/32 keys and\n"
        "16/16 sectors read\n"
        "Here is a guide to \nfollow to read \nMIFARE Classic:\n"
        "https://flipper.wiki/mifareclassic/\n"
        "Once completed, Scan again",
        message_title);

    widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, furi_string_get_cstr(parsed_data));

    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Exit", metroflip_exit_widget_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
    metroflip_app_blink_stop(app);
}

static NfcCommand
    metroflip_scene_smartrider_poller_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);
    furi_assert(event.event_data);
    furi_assert(event.protocol == NfcProtocolMfClassic);

    NfcCommand command = NfcCommandContinue;
    const MfClassicPollerEvent* mfc_event = event.event_data;
    Metroflip* app = context;
    FuriString* parsed_data = furi_string_alloc();
    Widget* widget = app->widget;

    if(mfc_event->type == MfClassicPollerEventTypeCardDetected) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventCardDetected);
        command = NfcCommandContinue;
    } else if(mfc_event->type == MfClassicPollerEventTypeCardLost) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventCardLost);

        command = NfcCommandStop;
    } else if(mfc_event->type == MfClassicPollerEventTypeRequestMode) {
        mfc_event->data->poller_mode.mode = MfClassicPollerModeRead;
        nfc_device_set_data(
            app->nfc_device, NfcProtocolMfClassic, nfc_poller_get_data(app->poller));
        size_t uid_len = 0;
        const uint8_t* uid = nfc_device_get_uid(app->nfc_device, &uid_len);
        /*-----------------All of this is to store a keyfile in a permanent way for the user to always access------------*/
        /*-----------------Open cache file (if exists)------------*/

        char uid_str[uid_len * 2 + 1];
        uid_to_string(uid, uid_len, uid_str, sizeof(uid_str));
        KeyfileManager manage = manage_keyfiles(uid_str);

        switch(manage) {
        case MISSING_KEYFILE:
            handle_keyfile_case(app, "No keys found", "Missing keyfile", parsed_data);
            command = NfcCommandStop;
            break;

        case INCOMPLETE_KEYFILE:
            handle_keyfile_case(app, "Incomplete keyfile", "incomplete keyfile", parsed_data);
            command = NfcCommandStop;
            break;

        case SUCCESSFUL:
            mf_classic_key_cache_load(app->mfc_key_cache, uid, uid_len);
            FURI_LOG_I(TAG, "success");
            break;
        }
    } else if(mfc_event->type == MfClassicPollerEventTypeRequestReadSector) {
        FURI_LOG_I(TAG, "sec_num: %d", sector_num);
        MfClassicKey key = {};
        MfClassicKeyType key_type = MfClassicKeyTypeA;
        if(mf_classic_key_cache_get_next_key(app->mfc_key_cache, &sector_num, &key, &key_type)) {
            mfc_event->data->read_sector_request_data.sector_num = sector_num;
            mfc_event->data->read_sector_request_data.key = key;
            mfc_event->data->read_sector_request_data.key_type = key_type;
            mfc_event->data->read_sector_request_data.key_provided = true;
        } else {
            mfc_event->data->read_sector_request_data.key_provided = false;
        }
    } else if(mfc_event->type == MfClassicPollerEventTypeSuccess) {
        nfc_device_set_data(
            app->nfc_device, NfcProtocolMfClassic, nfc_poller_get_data(app->poller));

        dolphin_deed(DolphinDeedNfcReadSuccess);
        furi_string_reset(app->text_box_store);
        if(!smartrider_parse(app->nfc_device, parsed_data)) {
            FURI_LOG_I(TAG, "Unknown card type");
            furi_string_printf(parsed_data, "\e#Unknown card\n");
        }
        widget_add_text_scroll_element(widget, 0, 0, 128, 64, furi_string_get_cstr(parsed_data));

        widget_add_button_element(
            widget, GuiButtonTypeRight, "Exit", metroflip_exit_widget_callback, app);

        furi_string_free(parsed_data);
        view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
        metroflip_app_blink_stop(app);
        UNUSED(smartrider_parse);
        command = NfcCommandStop;
    } else if(mfc_event->type == MfClassicPollerEventTypeFail) {
        FURI_LOG_I(TAG, "fail");
        command = NfcCommandStop;
    }

    return command;
}

void metroflip_scene_smartrider_on_enter(void* context) {
    Metroflip* app = context;
    dolphin_deed(DolphinDeedNfcRead);

    mf_classic_key_cache_reset(app->mfc_key_cache);

    // Setup view
    Popup* popup = app->popup;
    popup_set_header(popup, "Apply\n card to\nthe back", 68, 30, AlignLeft, AlignTop);
    popup_set_icon(popup, 0, 3, &I_RFIDDolphinReceive_97x61);

    // Start worker
    view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewPopup);
    nfc_scanner_alloc(app->nfc);
    app->poller = nfc_poller_alloc(app->nfc, NfcProtocolMfClassic);
    nfc_poller_start(app->poller, metroflip_scene_smartrider_poller_callback, app);

    metroflip_app_blink_start(app);
}

bool metroflip_scene_smartrider_on_event(void* context, SceneManagerEvent event) {
    Metroflip* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MetroflipCustomEventCardDetected) {
            Popup* popup = app->popup;
            popup_set_header(popup, "DON'T\nMOVE", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventCardLost) {
            Popup* popup = app->popup;
            popup_set_header(popup, "Card \n lost", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventWrongCard) {
            Popup* popup = app->popup;
            popup_set_header(popup, "WRONG \n CARD", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventPollerFail) {
            Popup* popup = app->popup;
            popup_set_header(popup, "Failed", 68, 30, AlignLeft, AlignTop);
            consumed = true;
        } else if(event.event == MetroflipCustomEventPollerSuccess) {
            scene_manager_next_scene(app->scene_manager, MetroflipSceneReadSuccess);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, MetroflipSceneStart);
        consumed = true;
    }

    return consumed;
}

void metroflip_scene_smartrider_on_exit(void* context) {
    Metroflip* app = context;
    widget_reset(app->widget);

    if(app->poller) {
        nfc_poller_stop(app->poller);
        nfc_poller_free(app->poller);
    }

    // Clear view
    popup_reset(app->popup);

    metroflip_app_blink_stop(app);
}
