#ifndef DEADLOCK_H
#define DEADLOCK_H

#include <stddef.h>
#include <stdbool.h>

#define MAX_P 12
#define MAX_R 16
#define NAME_LEN 16
#define MAX_PLANS 1024
#define MAX_OUTPUT_PLANS 30

typedef struct {
    char name[NAME_LEN];
    int completed;
    int restart;
    int cost;
    int priority;
    bool deadlocked;
    int holds[MAX_R];
    int holds_count;
    int waits[MAX_R];
    int waits_count;
} Process;

typedef struct {
    Process processes[MAX_P];
    int p_count;
    char res_names[MAX_R][NAME_LEN];
    int r_count;
    int owner[MAX_R]; /* Process index holding resource r, or -1 if free */
    int maxCost;
    int maxVictims;
} System;

typedef struct {
    int victims[MAX_P];
    int victim_count;
    int cost;
    int kills;
    bool withinLimits;
    int released[MAX_R];
    int released_count;
} RecoveryPlan;

typedef struct {
    char name[64];
    RecoveryPlan plan;
} BaselineResult;

typedef struct {
    int from;
    int to;
    int res_idx;
} Edge;

/* Analyze input text specification and return a heap-allocated JSON string.
   Caller must free() the returned pointer. */
char *analyze_json(const char *input_text);

#endif /* DEADLOCK_H */
