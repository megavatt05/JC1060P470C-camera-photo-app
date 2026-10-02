#ifndef HOST_TASK_H
#define HOST_TASK_H
typedef void (*TaskFunction_t)(void *);
int xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, int stack,
                            void *arg, int prio, void *handle, int core);
#endif
