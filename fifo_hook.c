#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

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

/* Replacement rule structure */
typedef struct {
    uint8_t message_type;
    uint8_t find_pattern[MAX_PATTERN_SIZE];
    uint8_t replace_pattern[MAX_PATTERN_SIZE];
    size_t pattern_len;
    int is_sequence;  /* 1 if this is a sequence rule (just MT + number) */
    int sequence_num; /* Sequence number for ME 0x00ab replacements */
} replacement_rule_t;

/* Global replacement rules */
static replacement_rule_t rules[MAX_REPLACEMENTS];
static int num_rules = 0;
static int rules_loaded = 0;

/* Counter for all-zeros replacements (for ordered replacements) */
static int zeros_replacement_counter = 0;

typedef void* (*memcpy_s_chk_fn)(void *dst, size_t dst_size, const void *src, 
                                  size_t n, size_t src_size, size_t overflow);

static memcpy_s_chk_fn original_memcpy_s_chk = NULL;

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

/* Convert hex string to bytes */
static size_t hex_string_to_bytes(const char *hex_str, uint8_t *bytes, size_t max_bytes) {
    size_t len = 0;
    const char *p = hex_str;
    
    /* Skip whitespace */
    while (*p && isspace(*p)) p++;
    
    while (*p && len < max_bytes) {
        /* Skip whitespace */
        while (*p && isspace(*p)) p++;
        if (!*p) break;
        
        /* Parse two hex digits */
        char hex[3] = {0};
        if (isxdigit(*p)) {
            hex[0] = *p++;
            if (isxdigit(*p)) {
                hex[1] = *p++;
            }
            bytes[len++] = (uint8_t)strtol(hex, NULL, 16);
        } else {
            p++;
        }
    }
    
    return len;
}

/* Load replacement rules from INI file */
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
        
        /* Skip empty lines and comments */
        char *p = line;
        while (*p && isspace(*p)) p++;
        if (!*p || *p == '#' || *p == ';')
            continue;
        
        /* Parse: <message_type> <find_hex> <replace_hex> OR <message_type> <sequence_num> */
        int msg_type, seq_num;
        char find_hex[MAX_PATTERN_SIZE * 2 + 1];
        char replace_hex[MAX_PATTERN_SIZE * 2 + 1];
        char third_field[MAX_PATTERN_SIZE * 2 + 1];
        
        /* Count fields in the line */
        int field_count = sscanf(line, "%d %s %s", &msg_type, find_hex, third_field);
        
        if (field_count == 2) {
            /* Two fields: MT SEQ_NUM (sequence format) */
            /* Re-parse to get sequence number */
            if (sscanf(line, "%d %d", &msg_type, &seq_num) == 2) {
                replacement_rule_t *rule = &rules[num_rules];
                
                rule->message_type = (uint8_t)msg_type;
                rule->is_sequence = 1;
                rule->sequence_num = seq_num;
                rule->pattern_len = 0;  /* Not used for sequence rules */
                
                fprintf(stderr, "[OMCI] Rule %d: MT=%d (%s), sequence=%d (ME 0x00ab auto-replace)\n",
                        num_rules + 1, rule->message_type, 
                        get_omci_msg_type_name(rule->message_type),
                        seq_num);
                
                num_rules++;
            } else {
                fprintf(stderr, "[OMCI] Warning: Line %d - invalid two-field format\n", line_num);
            }
        }
        else if (field_count == 3) {
            /* Three fields: MT FIND REPLACE (pattern format) */
            strcpy(replace_hex, third_field);
            
            replacement_rule_t *rule = &rules[num_rules];
            
            rule->message_type = (uint8_t)msg_type;
            rule->is_sequence = 0;
            rule->pattern_len = hex_string_to_bytes(find_hex, rule->find_pattern, MAX_PATTERN_SIZE);
            
            size_t replace_len = hex_string_to_bytes(replace_hex, rule->replace_pattern, MAX_PATTERN_SIZE);
            
            if (rule->pattern_len != replace_len) {
                fprintf(stderr, "[OMCI] Warning: Line %d - find and replace patterns have different lengths (%zu vs %zu)\n",
                        line_num, rule->pattern_len, replace_len);
                continue;
            }
            
            if (rule->pattern_len == 0) {
                fprintf(stderr, "[OMCI] Warning: Line %d - empty pattern\n", line_num);
                continue;
            }
            
            fprintf(stderr, "[OMCI] Rule %d: MT=%d (%s), pattern_len=%zu bytes\n",
                    num_rules + 1, rule->message_type, 
                    get_omci_msg_type_name(rule->message_type),
                    rule->pattern_len);
            
            num_rules++;
        } else {
            fprintf(stderr, "[OMCI] Warning: Line %d - invalid format (expected 'MT FIND REPLACE' or 'MT SEQ')\n", line_num);
        }
    }
    
    fclose(fp);
    fprintf(stderr, "[OMCI] Loaded %d replacement rules\n", num_rules);
    
    /* Debug: Print first 8 bytes of each rule's find pattern */
    for (int i = 0; i < num_rules; i++) {
        fprintf(stderr, "[OMCI] Rule %d: MT=%d, len=%zu, find=[", 
                i, rules[i].message_type, rules[i].pattern_len);
        for (size_t j = 0; j < 8 && j < rules[i].pattern_len; j++) {
            fprintf(stderr, "%02x ", rules[i].find_pattern[j]);
        }
        fprintf(stderr, "...]\n");
    }
}

/* Apply replacement rules to message */
static int apply_replacements(uint8_t *data, size_t len, uint8_t msg_type, uint16_t class_id) {
    int i;
    int replaced = 0;
    const size_t offset = 8;
    
    if (len < offset) {
        return 0;
    }
    
    /* Special handling for ME Class 0x00ab with message type 9 */
    if (class_id == 0x00ab && msg_type == 9) {
        int seq_rules_found = 0;
        int seq_rule_indices[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        
        for (i = 0; i < num_rules; i++) {
            replacement_rule_t *rule = &rules[i];
            if (rule->message_type == 9 && rule->is_sequence) {
                seq_rule_indices[seq_rules_found] = i;
                seq_rules_found++;
            }
        }
        
        if (seq_rules_found > 0) {
            int target_seq = zeros_replacement_counter + 1;
            for (i = 0; i < seq_rules_found; i++) {
                int rule_idx = seq_rule_indices[i];
                if (rule_idx >= 0 && rules[rule_idx].sequence_num == target_seq) {
                    uint8_t replacement[32] = {0};
                    
                    if (target_seq == 1) {
                        replacement[0] = 0x00;
                        replacement[1] = 0x38;
                        replacement[2] = 0x00;
                        replacement[3] = 0x81;
                        replacement[4] = 0x00;
                        replacement[5] = 0x81;
                    } else if (target_seq == 2) {
                        replacement[0] = 0x00;
                        replacement[1] = 0x01;
                    } else if (target_seq == 3) {
                        replacement[0] = 0x00;
                        replacement[1] = 0x44;
                        replacement[7] = 0x30;
                    }
                    
                    if (len >= offset + 32) {
                        memcpy(data + offset, replacement, 32);
                        zeros_replacement_counter++;
                        if (zeros_replacement_counter >= seq_rules_found) {
                            zeros_replacement_counter = 0;
                        }
                        replaced = 1;
                        return replaced;
                    }
                }
            }
        }
    }
    
    /* Normal pattern matching */
    for (i = 0; i < num_rules; i++) {
        replacement_rule_t *rule = &rules[i];
        
        if (rule->is_sequence) continue;
        if (rule->message_type != msg_type) continue;
        if (len < offset + rule->pattern_len) continue;
        
        if (memcmp(data + offset, rule->find_pattern, rule->pattern_len) == 0) {
            memcpy(data + offset, rule->replace_pattern, rule->pattern_len);
            replaced = 1;
            break;
        }
    }
    
    return replaced;
}

static void parse_and_modify_omci_message(void *dst, const void *src, size_t n) {
    uint8_t *data = (uint8_t *)dst;
    const uint8_t *src_data = (const uint8_t *)src;
    
    if (n < 8) {
        return;
    }
    
    /* Parse header */
    uint16_t trans_id = (src_data[0] << 8) | src_data[1];
    uint8_t type_byte = src_data[2];
    uint16_t class_id = (src_data[4] << 8) | src_data[5];
    uint16_t inst_id = (src_data[6] << 8) | src_data[7];
    uint8_t mt = type_byte & 0x1f;
    
    /* Apply replacements */
    int replaced = apply_replacements(data, n, mt, class_id);
    
    /* Only print if replacement occurred */
    if (replaced) {
        fprintf(stderr, "[OMCI] Replaced MT=%u TID=0x%04x Class=0x%04x Inst=0x%04x\n",
                mt, trans_id, class_id, inst_id);
        fflush(stderr);
    }
}

/* Hooked _memcpy_s_chk function */
void* _memcpy_s_chk(void *dst, size_t dst_size, const void *src, 
                    size_t n, size_t src_size, size_t overflow) {
    void *result;
    static int hook_call_count = 0;
    
    /* Load original function on first call */
    if (!original_memcpy_s_chk) {
        original_memcpy_s_chk = (memcpy_s_chk_fn)dlsym(RTLD_NEXT, "_memcpy_s_chk");
        if (!original_memcpy_s_chk) {
            fprintf(stderr, "[HOOK] Failed to load original _memcpy_s_chk\n");
            return NULL;
        }
        fprintf(stderr, "[HOOK] Successfully loaded original _memcpy_s_chk\n");
    }
    
    hook_call_count++;
    
    /* Log first few calls to verify hook is working */
    if (hook_call_count <= 5) {
        fprintf(stderr, "[HOOK] Call #%d: dst_size=%zu, src_size=%zu, n=%zu\n",
                hook_call_count, dst_size, src_size, n);
    }
    
    /* Call original function first to copy the data */
    result = original_memcpy_s_chk(dst, dst_size, src, n, src_size, overflow);
    
    /* Check if this is the OMCI message copy (1976 bytes = 0x7b8) */
    if (dst_size == 1976 && src_size == 1976) {
        fprintf(stderr, "[HOOK] OMCI buffer detected! (1976 bytes) n=%zu\n", n);
        
        if (src == NULL) {
            fprintf(stderr, "[HOOK] Warning: src is NULL\n");
            return result;
        }
        
        if (n < 8) {
            fprintf(stderr, "[HOOK] Warning: n=%zu is too small (need >= 8)\n", n);
            return result;
        }
        
        /* Load replacement rules if not already loaded */
        if (!rules_loaded) {
            fprintf(stderr, "[HOOK] Loading replacement rules...\n");
            load_replacement_rules();
        }
        
        if (num_rules == 0) {
            fprintf(stderr, "[HOOK] No replacement rules loaded\n");
        } else {
            fprintf(stderr, "[HOOK] Attempting to apply %d rules...\n", num_rules);
            /* Parse and potentially modify the message (dst now has the copied data) */
            parse_and_modify_omci_message(dst, src, n);
        }
    }
    
    return result;
}



/* Constructor */
__attribute__((constructor))
static void init_hook(void)
{
   fprintf(stderr, "\n");
   fprintf(stderr, "================================================\n");
   fprintf(stderr, "  IFX_FIFO Hook Library Loaded (MIPS 24Kc)\n");
   fprintf(stderr, "  Hooking: _memcpy_s_chk\n");
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