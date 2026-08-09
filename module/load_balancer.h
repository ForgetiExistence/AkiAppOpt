#ifndef AKIAPPOPT_LOAD_BALANCER_H
#define AKIAPPOPT_LOAD_BALANCER_H

#include <sched.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct LoadBalancer LoadBalancer;

typedef struct {
    cpu_set_t present_cpus;
    cpu_set_t e_core;
    cpu_set_t p_core;
    cpu_set_t hp_core;
} LoadBalancerTopology;

typedef bool (*LoadBalancerRuleMatch)(void *context, const char *package_name);

LoadBalancer *load_balancer_create(const char *game_list_file);
void load_balancer_destroy(LoadBalancer *balancer);
void load_balancer_reset(LoadBalancer *balancer, const cpu_set_t *fallback_cpus);

/* Sample unruled Android app threads and constrain them to suitable CPU tiers. */
void load_balancer_tick(LoadBalancer *balancer,
                        const LoadBalancerTopology *topology,
                        LoadBalancerRuleMatch rule_match,
                        void *rule_context);

#endif
