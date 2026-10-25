#include "deadlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* --- Dynamic String Buffer for JSON Building --- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb) {
    sb->cap = 1024;
    sb->len = 0;
    sb->data = (char *)malloc(sb->cap);
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

static void sb_grow(StrBuf *sb, size_t needed) {
    if (sb->len + needed + 1 > sb->cap) {
        while (sb->len + needed + 1 > sb->cap) {
            sb->cap *= 2;
        }
        char *new_data = (char *)realloc(sb->data, sb->cap);
        if (new_data) {
            sb->data = new_data;
        }
    }
}

static void sb_append(StrBuf *sb, const char *str) {
    if (!str) return;
    size_t slen = strlen(str);
    sb_grow(sb, slen);
    memcpy(sb->data + sb->len, str, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
}

static void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (needed > 0) {
        sb_grow(sb, (size_t)needed);
        vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, args_copy);
        sb->len += (size_t)needed;
    }
    va_end(args_copy);
}

static void sb_append_json_str(StrBuf *sb, const char *str) {
    sb_append(sb, "\"");
    if (!str) {
        sb_append(sb, "\"");
        return;
    }
    for (const char *p = str; *p; p++) {
        if (*p == '"') {
            sb_append(sb, "\\\"");
        } else if (*p == '\\') {
            sb_append(sb, "\\\\");
        } else if (*p == '\b') {
            sb_append(sb, "\\b");
        } else if (*p == '\f') {
            sb_append(sb, "\\f");
        } else if (*p == '\n') {
            sb_append(sb, "\\n");
        } else if (*p == '\r') {
            sb_append(sb, "\\r");
        } else if (*p == '\t') {
            sb_append(sb, "\\t");
        } else if ((unsigned char)*p < 0x20) {
            sb_printf(sb, "\\u%04x", (unsigned int)(unsigned char)*p);
        } else {
            char c[2] = {*p, '\0'};
            sb_append(sb, c);
        }
    }
    sb_append(sb, "\"");
}

static char *create_error_json(const char *msg) {
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"ok\":false,\"error\":");
    sb_append_json_str(&sb, msg);
    sb_append(&sb, "}");
    return sb.data;
}

/* --- Validation & Helper Functions --- */

static bool is_valid_name(const char *s) {
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len < 1 || len >= NAME_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') {
            return false;
        }
    }
    return true;
}

static bool parse_non_negative_int(const char *s, int *out) {
    if (!s || !*s) return false;
    char *endptr = NULL;
    long val = strtol(s, &endptr, 10);
    if (*endptr != '\0' || val < 0 || val > 1000000000) {
        return false;
    }
    *out = (int)val;
    return true;
}

static int get_or_add_resource(System *sys, const char *name, char *err_buf, size_t err_size) {
    for (int i = 0; i < sys->r_count; i++) {
        if (strcmp(sys->res_names[i], name) == 0) {
            return i;
        }
    }
    if (sys->r_count >= MAX_R) {
        snprintf(err_buf, err_size, "Resource limit exceeded (maximum %d resources allowed)", MAX_R);
        return -1;
    }
    int idx = sys->r_count++;
    strncpy(sys->res_names[idx], name, NAME_LEN - 1);
    sys->res_names[idx][NAME_LEN - 1] = '\0';
    sys->owner[idx] = -1;
    return idx;
}

static void trim_inplace(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

/* Splits a line into tokens by whitespace */
static int split_line_tokens(char *line, char *tokens[], int max_tokens) {
    int count = 0;
    char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (count >= max_tokens) return count + 1; /* More tokens than allowed */
        tokens[count++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    return count;
}

/* Parse comma-separated resource list, or "-" for none */
static bool parse_resource_list(System *sys, int p_idx, const char *spec, bool is_holds,
                               int *out_list, int *out_count, char *err_buf, size_t err_size) {
    *out_count = 0;
    if (strcmp(spec, "-") == 0) {
        return true;
    }

    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *token = strtok(buf, ",");
    while (token) {
        trim_inplace(token);
        if (!is_valid_name(token)) {
            snprintf(err_buf, err_size, "Invalid resource name '%s' in %s list for process '%s'",
                     token, is_holds ? "holds" : "waits", sys->processes[p_idx].name);
            return false;
        }

        /* Check duplicates in this list */
        for (int i = 0; i < *out_count; i++) {
            if (strcmp(sys->res_names[out_list[i]], token) == 0) {
                snprintf(err_buf, err_size, "Duplicate resource '%s' in %s list for process '%s'",
                         token, is_holds ? "holds" : "waits", sys->processes[p_idx].name);
                return false;
            }
        }

        int r_idx = get_or_add_resource(sys, token, err_buf, err_size);
        if (r_idx < 0) return false;

        if (is_holds) {
            if (sys->owner[r_idx] != -1) {
                snprintf(err_buf, err_size, "Resource '%s' is held by multiple processes ('%s' and '%s')",
                         token, sys->processes[sys->owner[r_idx]].name, sys->processes[p_idx].name);
                return false;
            }
            sys->owner[r_idx] = p_idx;
        }

        if (*out_count >= MAX_R) {
            snprintf(err_buf, err_size, "Resource list for process '%s' exceeds maximum allowed",
                     sys->processes[p_idx].name);
            return false;
        }

        out_list[(*out_count)++] = r_idx;
        token = strtok(NULL, ",");
    }

    if (*out_count == 0) {
        snprintf(err_buf, err_size, "Malformed resource list for process '%s'", sys->processes[p_idx].name);
        return false;
    }

    return true;
}

/* --- Simulation & Graph Algorithms --- */

static bool check_deadlock_subset(const System *sys, unsigned int victim_mask) {
    int sub_reach[MAX_P][MAX_P] = {{0}};

    for (int i = 0; i < sys->p_count; i++) {
        if (victim_mask & (1U << i)) continue;
        for (int w = 0; w < sys->processes[i].waits_count; w++) {
            int r = sys->processes[i].waits[w];
            int j = sys->owner[r];
            if (j != -1 && j != i && !(victim_mask & (1U << j))) {
                sub_reach[i][j] = 1;
            }
        }
    }

    /* Warshall's Transitive Closure */
    for (int k = 0; k < sys->p_count; k++) {
        if (victim_mask & (1U << k)) continue;
        for (int i = 0; i < sys->p_count; i++) {
            if (victim_mask & (1U << i)) continue;
            for (int j = 0; j < sys->p_count; j++) {
                if (victim_mask & (1U << j)) continue;
                if (sub_reach[i][k] && sub_reach[k][j]) {
                    sub_reach[i][j] = 1;
                }
            }
        }
    }

    /* A cycle exists if any surviving process can reach itself */
    for (int i = 0; i < sys->p_count; i++) {
        if (!(victim_mask & (1U << i)) && sub_reach[i][i]) {
            return true;
        }
    }
    return false;
}

static bool get_deadlocked_survivors(const System *sys, unsigned int victim_mask, bool out_deadlocked[MAX_P]) {
    int sub_reach[MAX_P][MAX_P] = {{0}};

    for (int i = 0; i < sys->p_count; i++) {
        out_deadlocked[i] = false;
        if (victim_mask & (1U << i)) continue;
        for (int w = 0; w < sys->processes[i].waits_count; w++) {
            int r = sys->processes[i].waits[w];
            int j = sys->owner[r];
            if (j != -1 && j != i && !(victim_mask & (1U << j))) {
                sub_reach[i][j] = 1;
            }
        }
    }

    for (int k = 0; k < sys->p_count; k++) {
        if (victim_mask & (1U << k)) continue;
        for (int i = 0; i < sys->p_count; i++) {
            if (victim_mask & (1U << i)) continue;
            for (int j = 0; j < sys->p_count; j++) {
                if (victim_mask & (1U << j)) continue;
                if (sub_reach[i][k] && sub_reach[k][j]) {
                    sub_reach[i][j] = 1;
                }
            }
        }
    }

    bool any = false;
    for (int i = 0; i < sys->p_count; i++) {
        if (!(victim_mask & (1U << i)) && sub_reach[i][i]) {
            out_deadlocked[i] = true;
            any = true;
        }
    }
    return any;
}

static void populate_released_resources(const System *sys, RecoveryPlan *plan) {
    bool res_marked[MAX_R] = {false};
    plan->released_count = 0;

    for (int i = 0; i < plan->victim_count; i++) {
        int v = plan->victims[i];
        for (int h = 0; h < sys->processes[v].holds_count; h++) {
            int r = sys->processes[v].holds[h];
            if (!res_marked[r]) {
                res_marked[r] = true;
                plan->released[plan->released_count++] = r;
            }
        }
    }

    /* Sort released resources for deterministic JSON output */
    for (int i = 0; i < plan->released_count - 1; i++) {
        for (int j = i + 1; j < plan->released_count; j++) {
            if (strcmp(sys->res_names[plan->released[i]], sys->res_names[plan->released[j]]) > 0) {
                int tmp = plan->released[i];
                plan->released[i] = plan->released[j];
                plan->released[j] = tmp;
            }
        }
    }
}

static int compare_plans(const void *a, const void *b) {
    const RecoveryPlan *p1 = (const RecoveryPlan *)a;
    const RecoveryPlan *p2 = (const RecoveryPlan *)b;

    /* 1. Feasible plans (within limits) come first */
    if (p1->withinLimits != p2->withinLimits) {
        return p1->withinLimits ? -1 : 1;
    }
    /* 2. Lowest cost first */
    if (p1->cost != p2->cost) {
        return (p1->cost < p2->cost) ? -1 : 1;
    }
    /* 3. Fewer victims first */
    if (p1->kills != p2->kills) {
        return (p1->kills < p2->kills) ? -1 : 1;
    }
    /* 4. Tie-break by victim indices */
    for (int i = 0; i < p1->victim_count && i < p2->victim_count; i++) {
        if (p1->victims[i] != p2->victims[i]) {
            return (p1->victims[i] < p2->victims[i]) ? -1 : 1;
        }
    }
    return 0;
}

static RecoveryPlan run_baseline(const System *sys, int strategy) {
    /* strategy 0: lowest individual cost
       strategy 1: lowest priority (tie-break by lower cost) */
    RecoveryPlan plan;
    memset(&plan, 0, sizeof(plan));

    unsigned int victim_mask = 0;

    while (true) {
        bool deadlocked[MAX_P] = {false};
        bool any = get_deadlocked_survivors(sys, victim_mask, deadlocked);
        if (!any) {
            break;
        }

        int best_candidate = -1;
        for (int i = 0; i < sys->p_count; i++) {
            if (!deadlocked[i]) continue;
            if (best_candidate == -1) {
                best_candidate = i;
                continue;
            }

            if (strategy == 0) {
                /* Lowest individual cost */
                if (sys->processes[i].cost < sys->processes[best_candidate].cost) {
                    best_candidate = i;
                } else if (sys->processes[i].cost == sys->processes[best_candidate].cost) {
                    /* Tie-break: lower priority, then index */
                    if (sys->processes[i].priority < sys->processes[best_candidate].priority) {
                        best_candidate = i;
                    } else if (sys->processes[i].priority == sys->processes[best_candidate].priority && i < best_candidate) {
                        best_candidate = i;
                    }
                }
            } else {
                /* Lowest priority (tie-break by lower cost) */
                if (sys->processes[i].priority < sys->processes[best_candidate].priority) {
                    best_candidate = i;
                } else if (sys->processes[i].priority == sys->processes[best_candidate].priority) {
                    if (sys->processes[i].cost < sys->processes[best_candidate].cost) {
                        best_candidate = i;
                    } else if (sys->processes[i].cost == sys->processes[best_candidate].cost && i < best_candidate) {
                        best_candidate = i;
                    }
                }
            }
        }

        if (best_candidate == -1) break;

        victim_mask |= (1U << best_candidate);
        plan.victims[plan.victim_count++] = best_candidate;
        plan.cost += sys->processes[best_candidate].cost;
    }

    plan.kills = plan.victim_count;
    plan.withinLimits = (plan.cost <= sys->maxCost && plan.kills <= sys->maxVictims);
    populate_released_resources(sys, &plan);

    return plan;
}

static void append_plan_json(StrBuf *sb, const System *sys, const RecoveryPlan *plan) {
    sb_append(sb, "{\"victims\":[");
    for (int i = 0; i < plan->victim_count; i++) {
        if (i > 0) sb_append(sb, ",");
        sb_append_json_str(sb, sys->processes[plan->victims[i]].name);
    }
    sb_printf(sb, "],\"cost\":%d,\"kills\":%d,\"withinLimits\":%s,\"released\":[",
              plan->cost, plan->kills, plan->withinLimits ? "true" : "false");
    for (int i = 0; i < plan->released_count; i++) {
        if (i > 0) sb_append(sb, ",");
        sb_append_json_str(sb, sys->res_names[plan->released[i]]);
    }
    sb_append(sb, "]}");
}

/* Pure C99 line iterator that modifies in-place and splits by \r and \n */
static char *get_next_line(char **cursor) {
    if (!cursor || !*cursor || !**cursor) return NULL;
    char *start = *cursor;
    char *p = start;
    while (*p && *p != '\r' && *p != '\n') {
        p++;
    }
    if (*p) {
        if (*p == '\r' && *(p + 1) == '\n') {
            *p = '\0';
            *cursor = p + 2;
        } else {
            *p = '\0';
            *cursor = p + 1;
        }
    } else {
        *cursor = p;
    }
    return start;
}

/* --- Main Analysis Entrypoint --- */

char *analyze_json(const char *input_text) {
    if (!input_text) {
        return create_error_json("No input provided");
    }

    System sys;
    memset(&sys, 0, sizeof(sys));
    for (int i = 0; i < MAX_R; i++) sys.owner[i] = -1;

    bool limits_found = false;
    char err_buf[256];

    /* Copy input to a mutable buffer for line iteration */
    size_t input_len = strlen(input_text);
    char *input_copy = (char *)malloc(input_len + 1);
    if (!input_copy) {
        return create_error_json("Memory allocation error");
    }
    memcpy(input_copy, input_text, input_len + 1);

    char *cursor = input_copy;
    char *line = get_next_line(&cursor);

    while (line) {
        trim_inplace(line);
        if (*line == '\0' || *line == '#') {
            line = get_next_line(&cursor);
            continue;
        }

        /* Check for LIMITS line */
        if (strncmp(line, "LIMITS", 6) == 0 && (line[6] == '\0' || isspace((unsigned char)line[6]))) {
            if (limits_found) {
                free(input_copy);
                return create_error_json("Duplicate LIMITS specification");
            }
            char *tokens[8];
            int tc = split_line_tokens(line, tokens, 8);
            if (tc != 3) {
                free(input_copy);
                return create_error_json("Malformed LIMITS line: expected 'LIMITS <maxCost> <maxVictims>'");
            }
            int mc = 0, mv = 0;
            if (!parse_non_negative_int(tokens[1], &mc) || !parse_non_negative_int(tokens[2], &mv)) {
                free(input_copy);
                return create_error_json("Invalid numeric values on LIMITS line (must be non-negative integers)");
            }
            sys.maxCost = mc;
            sys.maxVictims = mv;
            limits_found = true;
            line = get_next_line(&cursor);
            continue;
        }

        /* Process line */
        char *tokens[10];
        int tc = split_line_tokens(line, tokens, 10);
        if (tc != 6) {
            free(input_copy);
            return create_error_json("Malformed process line: expected 6 fields (<name> <completed> <restart> <priority> <holds> <waits>)");
        }

        if (sys.p_count >= MAX_P) {
            free(input_copy);
            snprintf(err_buf, sizeof(err_buf), "Process limit exceeded (maximum %d processes allowed)", MAX_P);
            return create_error_json(err_buf);
        }

        int p_idx = sys.p_count;
        Process *p = &sys.processes[p_idx];
        memset(p, 0, sizeof(*p));

        if (!is_valid_name(tokens[0])) {
            free(input_copy);
            snprintf(err_buf, sizeof(err_buf), "Invalid process name '%s' (1-15 alphanumeric/underscore characters required)", tokens[0]);
            return create_error_json(err_buf);
        }

        /* Check duplicate process name */
        for (int i = 0; i < sys.p_count; i++) {
            if (strcmp(sys.processes[i].name, tokens[0]) == 0) {
                free(input_copy);
                snprintf(err_buf, sizeof(err_buf), "Duplicate process name: '%s'", tokens[0]);
                return create_error_json(err_buf);
            }
        }
        strncpy(p->name, tokens[0], NAME_LEN - 1);
        p->name[NAME_LEN - 1] = '\0';

        if (!parse_non_negative_int(tokens[1], &p->completed)) {
            free(input_copy);
            snprintf(err_buf, sizeof(err_buf), "Invalid completed work for process '%s'", p->name);
            return create_error_json(err_buf);
        }
        if (!parse_non_negative_int(tokens[2], &p->restart)) {
            free(input_copy);
            snprintf(err_buf, sizeof(err_buf), "Invalid restart cost for process '%s'", p->name);
            return create_error_json(err_buf);
        }
        p->cost = p->completed + p->restart;

        if (!parse_non_negative_int(tokens[3], &p->priority)) {
            free(input_copy);
            snprintf(err_buf, sizeof(err_buf), "Invalid priority for process '%s'", p->name);
            return create_error_json(err_buf);
        }

        /* Parse holds */
        if (!parse_resource_list(&sys, p_idx, tokens[4], true, p->holds, &p->holds_count, err_buf, sizeof(err_buf))) {
            free(input_copy);
            return create_error_json(err_buf);
        }

        /* Parse waits */
        if (!parse_resource_list(&sys, p_idx, tokens[5], false, p->waits, &p->waits_count, err_buf, sizeof(err_buf))) {
            free(input_copy);
            return create_error_json(err_buf);
        }

        sys.p_count++;
        line = get_next_line(&cursor);
    }

    free(input_copy);

    if (!limits_found) {
        return create_error_json("Missing LIMITS specification (e.g. 'LIMITS 60 2')");
    }
    if (sys.p_count == 0) {
        return create_error_json("No processes found in input");
    }

    /* Build wait-for graph edges */
    Edge edges[MAX_P * MAX_R];
    int edge_count = 0;
    int adj[MAX_P][MAX_P] = {{0}};

    for (int i = 0; i < sys.p_count; i++) {
        for (int w = 0; w < sys.processes[i].waits_count; w++) {
            int r = sys.processes[i].waits[w];
            int j = sys->owner[r];
            /* Edge Pi -> Pj if Pi waits for resource held by Pj (ignore self-edges and free resources) */
            if (j != -1 && j != i) {
                edges[edge_count].from = i;
                edges[edge_count].to = j;
                edges[edge_count].res_idx = r;
                edge_count++;
                adj[i][j] = 1;
            }
        }
    }

    /* Deadlock Detection via Transitive Closure (Warshall) */
    int reach[MAX_P][MAX_P] = {{0}};
    for (int i = 0; i < sys.p_count; i++) {
        for (int j = 0; j < sys.p_count; j++) {
            reach[i][j] = adj[i][j];
        }
    }

    for (int k = 0; k < sys.p_count; k++) {
        for (int i = 0; i < sys.p_count; i++) {
            for (int j = 0; j < sys.p_count; j++) {
                if (reach[i][k] && reach[k][j]) {
                    reach[i][j] = 1;
                }
            }
        }
    }

    bool has_deadlock = false;
    for (int i = 0; i < sys.p_count; i++) {
        if (reach[i][i]) {
            sys.processes[i].deadlocked = true;
            has_deadlock = true;
        } else {
            sys.processes[i].deadlocked = false;
        }
    }

    /* Recovery Planning */
    RecoveryPlan candidate_plans[MAX_PLANS];
    int plan_count = 0;
    int evaluated = 0;

    if (has_deadlock) {
        unsigned int total_masks = (1U << sys.p_count);
        for (unsigned int mask = 1; mask < total_masks; mask++) {
            evaluated++;
            if (!check_deadlock_subset(&sys, mask)) {
                /* Resolves deadlock. Check minimality: removing ANY one victim must restore deadlock */
                bool is_minimal = true;
                for (int v = 0; v < sys.p_count; v++) {
                    if (mask & (1U << v)) {
                        unsigned int reduced_mask = mask & ~(1U << v);
                        if (!check_deadlock_subset(&sys, reduced_mask)) {
                            is_minimal = false;
                            break;
                        }
                    }
                }

                if (is_minimal && plan_count < MAX_PLANS) {
                    RecoveryPlan *p = &candidate_plans[plan_count++];
                    memset(p, 0, sizeof(*p));
                    for (int v = 0; v < sys.p_count; v++) {
                        if (mask & (1U << v)) {
                            p->victims[p->victim_count++] = v;
                            p->cost += sys.processes[v].cost;
                        }
                    }
                    p->kills = p->victim_count;
                    p->withinLimits = (p->cost <= sys.maxCost && p->kills <= sys.maxVictims);
                    populate_released_resources(&sys, p);
                }
            }
        }

        if (plan_count > 1) {
            qsort(candidate_plans, (size_t)plan_count, sizeof(RecoveryPlan), compare_plans);
        }
    }

    /* Determine best plan: lowest total cost among feasible plans (tie-break fewer victims) */
    const RecoveryPlan *best = NULL;
    if (has_deadlock && plan_count > 0 && candidate_plans[0].withinLimits) {
        best = &candidate_plans[0];
    }

    /* Two baselines: only if deadlock exists */
    BaselineResult baselines[2];
    int baseline_count = 0;
    if (has_deadlock) {
        strcpy(baselines[0].name, "Lowest individual cost");
        baselines[0].plan = run_baseline(&sys, 0);

        strcpy(baselines[1].name, "Lowest priority");
        baselines[1].plan = run_baseline(&sys, 1);

        baseline_count = 2;
    }

    /* --- Build JSON Output --- */
    StrBuf sb;
    sb_init(&sb);

    sb_printf(&sb, "{\"ok\":true,\"deadlock\":%s,\"limits\":{\"maxCost\":%d,\"maxVictims\":%d},",
              has_deadlock ? "true" : "false", sys.maxCost, sys.maxVictims);

    /* Processes */
    sb_append(&sb, "\"processes\":[");
    for (int i = 0; i < sys.p_count; i++) {
        if (i > 0) sb_append(&sb, ",");
        const Process *p = &sys.processes[i];
        sb_printf(&sb, "{\"name\":");
        sb_append_json_str(&sb, p->name);
        sb_printf(&sb, ",\"completed\":%d,\"restart\":%d,\"cost\":%d,\"priority\":%d,\"deadlocked\":%s,\"holds\":[",
                  p->completed, p->restart, p->cost, p->priority, p->deadlocked ? "true" : "false");
        for (int h = 0; h < p->holds_count; h++) {
            if (h > 0) sb_append(&sb, ",");
            sb_append_json_str(&sb, sys.res_names[p->holds[h]]);
        }
        sb_append(&sb, "],\"waits\":[");
        for (int w = 0; w < p->waits_count; w++) {
            if (w > 0) sb_append(&sb, ",");
            sb_append_json_str(&sb, sys.res_names[p->waits[w]]);
        }
        sb_append(&sb, "]}");
    }
    sb_append(&sb, "],");

    /* Edges */
    sb_append(&sb, "\"edges\":[");
    for (int i = 0; i < edge_count; i++) {
        if (i > 0) sb_append(&sb, ",");
        sb_append(&sb, "{\"from\":");
        sb_append_json_str(&sb, sys.processes[edges[i].from].name);
        sb_append(&sb, ",\"to\":");
        sb_append_json_str(&sb, sys.processes[edges[i].to].name);
        sb_append(&sb, ",\"res\":");
        sb_append_json_str(&sb, sys.res_names[edges[i].res_idx]);
        sb_append(&sb, "}");
    }
    sb_append(&sb, "],");

    /* Plans (up to MAX_OUTPUT_PLANS = 30) */
    sb_append(&sb, "\"plans\":[");
    int output_plans = (plan_count > MAX_OUTPUT_PLANS) ? MAX_OUTPUT_PLANS : plan_count;
    for (int i = 0; i < output_plans; i++) {
        if (i > 0) sb_append(&sb, ",");
        append_plan_json(&sb, &sys, &candidate_plans[i]);
    }
    sb_append(&sb, "],");

    /* Best Plan */
    if (best) {
        sb_append(&sb, "\"best\":");
        append_plan_json(&sb, &sys, best);
        sb_append(&sb, ",");
    } else {
        sb_append(&sb, "\"best\":null,");
    }

    /* Baselines */
    sb_append(&sb, "\"baselines\":[");
    for (int i = 0; i < baseline_count; i++) {
        if (i > 0) sb_append(&sb, ",");
        sb_append(&sb, "{\"name\":");
        sb_append_json_str(&sb, baselines[i].name);
        sb_append(&sb, ",\"plan\":");
        append_plan_json(&sb, &sys, &baselines[i].plan);
        sb_append(&sb, "}");
    }
    sb_append(&sb, "],");

    /* Evaluated Subsets */
    sb_printf(&sb, "\"evaluated\":%d}", evaluated);

    return sb.data;
}
