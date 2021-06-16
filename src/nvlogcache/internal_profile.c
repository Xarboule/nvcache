#ifdef INTERNAL_PROFILE
#include "internal_profile.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "list.h"
#include "nvinfo.h"

struct node {
    double duration;
    list_head list;
};

struct perfs {
    struct timespec start, end;
    enum perfindex transfer;
    list_head listhead;
};

struct perfs stats[PERF_TOTAL];
char *perf_names[] = {"Cache hits", "Cache misses", "Dirty misses"};

//-----------------------------------------------
void chrono_start(enum perfindex e) {
    stats[e].transfer = PERF_TOTAL;
    clock_gettime(CLOCK_MONOTONIC, &stats[e].start);
}

//-----------------------------------------------
void chrono_stop(enum perfindex e) {
    enum perfindex chrono =
        stats[e].transfer == PERF_TOTAL ? e : stats[e].transfer;
    clock_gettime(CLOCK_MONOTONIC, &stats[chrono].end);

    struct node *n = malloc(sizeof(struct node));
    n->duration = (stats[chrono].end.tv_sec - stats[chrono].start.tv_sec) * 1e9;
    n->duration = (n->duration +
                   (stats[chrono].end.tv_nsec - stats[chrono].start.tv_nsec)) *
                  1e-3;
    list_add_tail(&n->list, &stats[chrono].listhead);
}

//-----------------------------------------------
void chrono_transfer(enum perfindex from, enum perfindex to) {
    stats[to].start = stats[from].start;
    stats[from].transfer = to;
}

//-----------------------------------------------
void perfs_init() {
    for (int i = 0; i < PERF_TOTAL; ++i) {
        INIT_LIST_HEAD(&stats[i].listhead);
    }
}

//-----------------------------------------------
void perfs_printall() {
    for (int i = 0; i < PERF_TOTAL; ++i) {
        printinfo(NVINFO, "\t%s", perf_names[i]);
        perfs_print(i);
    }
}

//-----------------------------------------------
// Merges two sorted linked lists
// *first gets the result, *second becomes empty
//-----------------------------------------------
list_head *merge(list_head *first, list_head *second) {
    if (list_empty(first)) {
        list_splice_init(second, first);  // swap
        return first;
    }

    if (list_empty(second)) return first;
    list_head *pos1 = first->next, *n1 = pos1->next, *pos2 = second->next,
              *n2 = pos2->next;
    while (pos1 != first) {
        struct node *f = list_entry(pos1, struct node, list),
                    *s = list_entry(pos2, struct node, list);
        if (f->duration < s->duration) {
            pos1 = n1;
            n1 = pos1->next;
        } else {
            list_move_tail(pos2, pos1);
            pos2 = n2;
            n2 = pos2->next;
        }
        if (pos2 == second) break;
    }

    if (!list_empty(second)) {
        list_splice_tail_init(second, first);  // concat
    }
    return first;
}

//-----------------------------------------------
list_head *median(list_head *head) {
    list_head *fast = head, *slow = head;
    while (fast->next != head && fast->next->next != head) {
        fast = fast->next->next;
        slow = slow->next;
    }
    return slow;
}

//-----------------------------------------------
// Split a doubly linked list into 2 halves
void split(list_head *head, list_head *ret) {
    list_head *med = median(head);
    list_cut_position(ret, head, med);
}

//-----------------------------------------------
list_head *mergeSort(list_head *head) {
    if (list_empty(head) || list_is_singular(head)) return head;

    LIST_HEAD(second);
    split(head, &second);

    // Recur for left and right halves
    mergeSort(head);
    mergeSort(&second);

    // Merge the two sorted halves
    return merge(head, &second);
}

//-----------------------------------------------
void perfs_print(enum perfindex e) {
    list_head *pos, *aux;
    double sum = 0, sumsq = 0;
    int count = 0;
#ifndef CDF_NOT_SORTED
    mergeSort(&stats[e].listhead);
    list_head *med = median(&stats[e].listhead);
    printinfo(NVINFO, "Median (us): %.9f",
              med ? list_entry(med, struct node, list)->duration : NAN);
#endif
#ifdef CDF
    char fname[100];
    snprintf(fname, sizeof(fname), "stats%02d.dat", e);
    FILE *fstats = musl_fopen(fname, "w");
#endif

    list_for_each_safe(pos, aux, &stats[e].listhead) {
        struct node *n = list_entry(pos, struct node, list);
        double sample = n->duration;

#ifdef CDF
        fprintf(fstats, "%f\n", sample);
#endif
#ifndef CDF_NOT_SORTED
        sum += sample;
        sumsq += sample * sample;
        ++count;
        list_del(pos);
        free(n);
    
#endif
    }
#ifdef CDF
    fclose(fstats);
#endif
#ifndef CDF_NOT_SORTED
    printinfo(NVINFO, "Average (us): %.9f", sum / count);
    double variance = (sumsq - (sum * sum) / count) / (count - 1);
    printinfo(NVINFO, "Stdev (us): %.9f", sqrt(variance));
    printinfo(NVINFO, "Samples: %d\n", count);
    // printinfo(NVINFO, "Variance (us²): %.9f", variance);
#endif
}
#endif
