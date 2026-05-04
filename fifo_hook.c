#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef uint32_t IFX_ulong_t;

#define MAX_REPLACEMENTS 64
#define MAX_PATTERN_SIZE 256
#define INI_FILE_PATH "/ptconf/8311/replacements.ini"

/* OMCI Message Type definitions from G.988 */
#define OMCI_MT_CREATE              4
#define OMCI_MT_DELETE              6
#define OMCI_MT_SET                 8
#define OMCI_MT_GET                 9
#define OMCI_MT_GET_ALL_ALARMS      11
#define OMCI_MT_GET_ALL_ALARMS_NEXT 12
#define OMCI_MT_MIB_UPLOAD          13
#define OMCI_MT_MIB_UPLOAD_NEXT     14
#define OMCI_MT_MIB_RESET           15
#define OMCI_MT_ALARM               16
#define OMCI_MT_AVC                 17
#define OMCI_MT_TEST                18
#define OMCI_MT_START_SW_DL         19
#define OMCI_MT_DL_SECTION          20
#define OMCI_MT_END_SW_DL           21
#define OMCI_MT_ACTIVATE_SW         22
#define OMCI_MT_COMMIT_SW           23
#define OMCI_MT_SYNC_TIME           24
#define OMCI_MT_REBOOT              25
#define OMCI_MT_GET_NEXT            26
#define OMCI_MT_TEST_RESULT         27
#define OMCI_MT_GET_CURRENT_DATA    28
#define OMCI_MT_SET_TABLE           29

/* OMCI type byte bits (byte 2 of message) */
#define OMCI_AR_BIT 0x40  /* Acknowledge Request - set on OLT->ONU requests (RX) */
#define OMCI_AK_BIT 0x20  /* Acknowledge - set on ONU->OLT responses (TX) */

#define DIR_TX 'T'
#define DIR_RX 'R'

/* Replacement rule structure.
 *
 * Each pattern has a parallel mask array: mask[i] = 1 means the byte at
 * position i is literal; mask[i] = 0 means it's a wildcard. In the find
 * pattern, wildcards match any incoming byte. In the replace pattern,
 * wildcards leave the destination byte unchanged. The hex token "??" in
 * the config produces a wildcard. */
typedef struct {
    char direction;       /* 'T' (ONU->OLT response) or 'R' (OLT->ONU request) */
    uint8_t message_type;
    uint8_t find_pattern[MAX_PATTERN_SIZE];
    uint8_t find_mask[MAX_PATTERN_SIZE];
    uint8_t replace_pattern[MAX_PATTERN_SIZE];
    uint8_t replace_mask[MAX_PATTERN_SIZE];
    size_t pattern_len;
} replacement_rule_t;

/* Global replacement rules */
static replacement_rule_t rules[MAX_REPLACEMENTS];
static int num_rules = 0;
static int rules_loaded = 0;

typedef void* (*memcpy_s_chk_fn)(void *dst, size_t dst_size, const void *src,
                                  size_t n, size_t src_size, size_t overflow);
typedef IFX_ulong_t* (*ifx_fifo_read_element_fn)(void *pFifo);

static memcpy_s_chk_fn original_memcpy_s_chk = NULL;
static ifx_fifo_read_element_fn original_ifx_fifo_read_element = NULL;

/* Helper function to get message type name */
static const char* get_omci_msg_type_name(uint8_t mt) {
    switch(mt) {
        case OMCI_MT_CREATE: return "CREATE";
        case OMCI_MT_DELETE: return "DELETE";
        case OMCI_MT_SET: return "SET";
        case OMCI_MT_GET: return "GET";
        case OMCI_MT_GET_ALL_ALARMS: return "GET_ALL_ALARMS";
        case OMCI_MT_GET_ALL_ALARMS_NEXT: return "GET_ALL_ALARMS_NEXT";
        case OMCI_MT_MIB_UPLOAD: return "MIB_UPLOAD";
        case OMCI_MT_MIB_UPLOAD_NEXT: return "MIB_UPLOAD_NEXT";
        case OMCI_MT_MIB_RESET: return "MIB_RESET";
        case OMCI_MT_ALARM: return "ALARM";
        case OMCI_MT_AVC: return "AVC";
        case OMCI_MT_TEST: return "TEST";
        case OMCI_MT_START_SW_DL: return "START_SW_DOWNLOAD";
        case OMCI_MT_DL_SECTION: return "DOWNLOAD_SECTION";
        case OMCI_MT_END_SW_DL: return "END_SW_DOWNLOAD";
        case OMCI_MT_ACTIVATE_SW: return "ACTIVATE_SOFTWARE";
        case OMCI_MT_COMMIT_SW: return "COMMIT_SOFTWARE";
        case OMCI_MT_SYNC_TIME: return "SYNC_TIME";
        case OMCI_MT_REBOOT: return "REBOOT";
        case OMCI_MT_GET_NEXT: return "GET_NEXT";
        case OMCI_MT_TEST_RESULT: return "TEST_RESULT";
        case OMCI_MT_GET_CURRENT_DATA: return "GET_CURRENT_DATA";
        case OMCI_MT_SET_TABLE: return "SET_TABLE";
        default: return "UNKNOWN";
    }
}

/* Convert hex string to bytes plus a parallel mask.
 * "??" is recognised as a wildcard byte: bytes[i] is set to 0 and
 * mask[i] is set to 0. Literal hex bytes get mask[i] = 1. */
static size_t hex_string_to_bytes(const char *hex_str, uint8_t *bytes,
                                  uint8_t *mask, size_t max_bytes) {
    size_t len = 0;
    const char *p = hex_str;

    while (*p && len < max_bytes) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (p[0] == '?' && p[1] == '?') {
            bytes[len] = 0;
            mask[len] = 0;
            len++;
            p += 2;
        } else if (isxdigit((unsigned char)*p)) {
            char hex[3] = {0};
            hex[0] = *p++;
            if (isxdigit((unsigned char)*p)) {
                hex[1] = *p++;
            }
            bytes[len] = (uint8_t)strtol(hex, NULL, 16);
            mask[len] = 1;
            len++;
        } else {
            p++;
        }
    }

    return len;
}

/* Load replacement rules from INI file
 *
 * Format: <R|T> <message_type> <find_hex> <replace_hex>
 *   R = receive  (OLT -> ONU request,  AR bit set)
 *   T = transmit (ONU -> OLT response, AK bit set)
 *
 * Patterns start at offset 4 in the OMCI message (the class_id field),
 * so they cover the 4-byte ME header (class_id + instance_id) plus payload.
 * A typical rule pattern is therefore 36 bytes (72 hex chars).
 */
static void load_replacement_rules(void) {
    FILE *fp;
    char line[1024];
    int line_num = 0;

    if (rules_loaded)
        return;

    rules_loaded = 1;
    num_rules = 0;

    fp = fopen(INI_FILE_PATH, "r");
    if (!fp) {
        fprintf(stderr, "[OMCI] No replacement rules file found at %s\n", INI_FILE_PATH);
        return;
    }

    fprintf(stderr, "[OMCI] Loading replacement rules from %s\n", INI_FILE_PATH);

    while (fgets(line, sizeof(line), fp) && num_rules < MAX_REPLACEMENTS) {
        line_num++;

        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#' || *p == ';')
            continue;

        char dir_str[8];
        int msg_type;
        char find_hex[MAX_PATTERN_SIZE * 2 + 1];
        char replace_hex[MAX_PATTERN_SIZE * 2 + 1];

        int field_count = sscanf(line, "%7s %d %s %s",
                                 dir_str, &msg_type, find_hex, replace_hex);

        if (field_count != 4) {
            fprintf(stderr, "[OMCI] Warning: Line %d - invalid format "
                            "(expected '<R|T> MT FIND REPLACE')\n", line_num);
            continue;
        }

        char direction = (char)toupper((unsigned char)dir_str[0]);
        if (direction != DIR_TX && direction != DIR_RX) {
            fprintf(stderr, "[OMCI] Warning: Line %d - direction must be R or T (got '%s')\n",
                    line_num, dir_str);
            continue;
        }

        replacement_rule_t *rule = &rules[num_rules];
        rule->direction = direction;
        rule->message_type = (uint8_t)msg_type;
        rule->pattern_len = hex_string_to_bytes(find_hex, rule->find_pattern,
                                                rule->find_mask, MAX_PATTERN_SIZE);

        size_t replace_len = hex_string_to_bytes(replace_hex, rule->replace_pattern,
                                                 rule->replace_mask, MAX_PATTERN_SIZE);

        if (rule->pattern_len != replace_len) {
            fprintf(stderr, "[OMCI] Warning: Line %d - find and replace lengths differ (%zu vs %zu)\n",
                    line_num, rule->pattern_len, replace_len);
            continue;
        }

        if (rule->pattern_len == 0) {
            fprintf(stderr, "[OMCI] Warning: Line %d - empty pattern\n", line_num);
            continue;
        }

        fprintf(stderr, "[OMCI] Rule %d: dir=%c MT=%d (%s) len=%zu bytes\n",
                num_rules + 1, rule->direction, rule->message_type,
                get_omci_msg_type_name(rule->message_type), rule->pattern_len);

        num_rules++;
    }

    fclose(fp);
    fprintf(stderr, "[OMCI] Loaded %d replacement rules\n", num_rules);

    for (int i = 0; i < num_rules; i++) {
        fprintf(stderr, "[OMCI] Rule %d: dir=%c MT=%d len=%zu find=[",
                i, rules[i].direction, rules[i].message_type, rules[i].pattern_len);
        for (size_t j = 0; j < 8 && j < rules[i].pattern_len; j++) {
            fprintf(stderr, "%02x ", rules[i].find_pattern[j]);
        }
        fprintf(stderr, "...]\n");
    }
}

/* Apply replacement rules to message.
 * Patterns match starting at offset 4 (the class_id field), so the rule
 * pattern naturally includes the 4-byte ME header followed by payload.
 */
static int apply_replacements(uint8_t *data, size_t len, uint8_t msg_type, char direction) {
    const size_t offset = 4;

    if (len < offset) {
        return 0;
    }

    for (int i = 0; i < num_rules; i++) {
        replacement_rule_t *rule = &rules[i];

        if (rule->direction != direction) continue;
        if (rule->message_type != msg_type) continue;
        if (len < offset + rule->pattern_len) continue;

        int matched = 1;
        for (size_t j = 0; j < rule->pattern_len; j++) {
            if (rule->find_mask[j] && data[offset + j] != rule->find_pattern[j]) {
                matched = 0;
                break;
            }
        }
        if (!matched) continue;

        for (size_t j = 0; j < rule->pattern_len; j++) {
            if (rule->replace_mask[j]) {
                data[offset + j] = rule->replace_pattern[j];
            }
        }
        return 1;
    }

    return 0;
}

static void parse_and_modify_omci_message(void *dst, const void *src, size_t n) {
    uint8_t *data = (uint8_t *)dst;
    const uint8_t *src_data = (const uint8_t *)src;

    if (n < 8) {
        return;
    }

    uint16_t trans_id = (src_data[0] << 8) | src_data[1];
    uint8_t type_byte = src_data[2];
    uint16_t class_id = (src_data[4] << 8) | src_data[5];
    uint16_t inst_id = (src_data[6] << 8) | src_data[7];
    uint8_t mt = type_byte & 0x1f;

    /* Determine direction from AR/AK bits in the OMCI type byte.
     * AR set => OLT->ONU request (RX)
     * AK set => ONU->OLT response (TX)
     * Neither set is unusual; skip rather than guess. */
    char direction;
    if (type_byte & OMCI_AK_BIT) {
        direction = DIR_TX;
    } else if (type_byte & OMCI_AR_BIT) {
        direction = DIR_RX;
    } else {
        return;
    }

    int replaced = apply_replacements(data, n, mt, direction);

    if (replaced) {
        fprintf(stderr, "[OMCI] Replaced dir=%c MT=%u TID=0x%04x Class=0x%04x Inst=0x%04x\n",
                direction, mt, trans_id, class_id, inst_id);
        fflush(stderr);
    }
}

/* Hooked _memcpy_s_chk function */
void* _memcpy_s_chk(void *dst, size_t dst_size, const void *src,
                    size_t n, size_t src_size, size_t overflow) {
    void *result;
    static int hook_call_count = 0;

    if (!original_memcpy_s_chk) {
        original_memcpy_s_chk = (memcpy_s_chk_fn)dlsym(RTLD_NEXT, "_memcpy_s_chk");
        if (!original_memcpy_s_chk) {
            fprintf(stderr, "[HOOK] Failed to load original _memcpy_s_chk\n");
            return NULL;
        }
        fprintf(stderr, "[HOOK] Successfully loaded original _memcpy_s_chk\n");
    }

    hook_call_count++;

    if (hook_call_count <= 5) {
        fprintf(stderr, "[HOOK] Call #%d: dst_size=%zu, src_size=%zu, n=%zu\n",
                hook_call_count, dst_size, src_size, n);
    }

    result = original_memcpy_s_chk(dst, dst_size, src, n, src_size, overflow);

    /* TX path: omci_msg_send copies into a 0x7b8 stack buffer. */
    if (dst_size != 0x7b8 || src_size != 0x7b8 || src == NULL || n < 8) {
        return result;
    }

    if (!rules_loaded) {
        load_replacement_rules();
    }

    if (num_rules > 0) {
        parse_and_modify_omci_message(dst, src, n);
    }

    return result;
}

/* Hooked IFX_Fifo_readElement — RX path.
 *
 * The FIFO stores pointers to pointers; dereferencing the returned slot
 * once yields the OMCI message buffer that omcid is about to consume.
 * Mutating that buffer in-place rewrites the request before it's parsed.
 */
IFX_ulong_t* IFX_Fifo_readElement(void *pFifo) {
    if (!original_ifx_fifo_read_element) {
        original_ifx_fifo_read_element =
            (ifx_fifo_read_element_fn)dlsym(RTLD_NEXT, "IFX_Fifo_readElement");
        if (!original_ifx_fifo_read_element) {
            fprintf(stderr, "[HOOK] Failed to load original IFX_Fifo_readElement\n");
            return NULL;
        }
        fprintf(stderr, "[HOOK] Successfully loaded original IFX_Fifo_readElement\n");
    }

    IFX_ulong_t *result = original_ifx_fifo_read_element(pFifo);
    if (result == NULL) {
        return result;
    }

    void *msg = *(void **)result;
    if (msg == NULL) {
        return result;
    }

    /* Sanity-check the pointer: aligned and in a plausible userspace range. */
    uintptr_t addr = (uintptr_t)msg;
    if ((addr & 0x3) || addr < 0x10000 || addr >= 0x80000000) {
        return result;
    }

    if (!rules_loaded) {
        load_replacement_rules();
    }

    if (num_rules == 0) {
        return result;
    }

    /* The FIFO doesn't tell us the message length. OMCI baseline messages
     * are 44 bytes (8 header + 32 payload + 4 trailer); extended can be
     * larger. Pass a generous upper bound so a 36-byte rule pattern fits
     * but we don't read past the buffer in the common case. */
    parse_and_modify_omci_message(msg, msg, 0x7b8);

    return result;
}


/* Constructor */
__attribute__((constructor))
static void init_hook(void)
{
   fprintf(stderr, "\n");
   fprintf(stderr, "================================================\n");
   fprintf(stderr, "  IFX_FIFO Hook Library Loaded (MIPS 24Kc)\n");
   fprintf(stderr, "  Hooking: _memcpy_s_chk (TX), IFX_Fifo_readElement (RX)\n");
   fprintf(stderr, "================================================\n");
   fprintf(stderr, "\n");
   fflush(stderr);
}

/* Destructor */
__attribute__((destructor))
static void fini_hook(void)
{
   fprintf(stderr, "\n");
   fprintf(stderr, "================================================\n");
   fprintf(stderr, "  IFX_FIFO Hook Library Unloaded\n");
   fprintf(stderr, "================================================\n");
   fprintf(stderr, "\n");
   fflush(stderr);
}
