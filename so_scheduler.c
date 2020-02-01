#include"so_scheduler.h"
#include<stdio.h>
#include<stdlib.h>


typedef struct thread {
	unsigned int priority;
	HANDLE tid;
	so_handler *func;
	HANDLE mutex_pl;
	unsigned int time_quantum;
	struct thread *urm;
} ThreadInfo;

typedef struct {
	unsigned int time_quantum;
	unsigned int io;
} scheduler;

typedef struct {
	so_handler *func;
	unsigned int priority;
	ThreadInfo *t;
} param;

scheduler *sch;
ThreadInfo **readyQueue;
ThreadInfo **blockingQueue;
ThreadInfo *running;
ThreadInfo *terminated;
ThreadInfo *last_thread;

DWORD myThread;

void insertQueueLast(ThreadInfo *ti)
{
	ThreadInfo **tl = &readyQueue[ti->priority];
	
	//pun ti la finalul listei
	for (; *tl != NULL; tl = &((*tl)->urm))
		;
	ti->urm = *tl;
	*tl = ti;
}

void insertQueueFirst(ThreadInfo *ti)
{
	ThreadInfo **tl = &readyQueue[ti->priority];
	
	//pun ti la inceputul listei
	ti->urm = *tl;
	*tl = ti;
}

ThreadInfo *extractQueue(void)
{
	ThreadInfo *p;
	int i, ok = 0;

	//extrag primul element din lista cu prio cea mai mare
	for (i = SO_MAX_PRIO; i >= 0; i--) {
		if (readyQueue[i] != NULL) {
			ok = 1;
			break;
		}
	}
	//daca nu gasesc nimic (READY e gol) intorc NULL
	if (ok == 0)
		return NULL;
	p = readyQueue[i];
	readyQueue[i] = readyQueue[i]->urm;
	return p;
}

void reschedule(void)
{
	ThreadInfo *thread = TlsGetValue(myThread);
	ThreadInfo *extr;

	thread->time_quantum--;
	if (thread->time_quantum > 0) {
		//daca a venit un thread cu prioritate mai mare
		ThreadInfo *extr = extractQueue();

		if (extr != NULL) {
			if (running->priority < extr->priority) {
				ThreadInfo *t = running;

				running = extr;
				insertQueueLast(t);
			} else {
				insertQueueFirst(extr);
			}
			ReleaseSemaphore(running->mutex_pl, 1, 0);
			WaitForSingleObject(thread->mutex_pl, INFINITE);
		}
		return;
	}

	//daca thread-ului curent i-a expirat cuanta
	thread->time_quantum = sch->time_quantum;

	//apare in coada READY o prioritate mai mare
	extr = extractQueue();
	if (extr != NULL) {
		if (running->priority <= extr->priority) {
			ThreadInfo *t = running;

			running = extr;
			insertQueueLast(t);
		} else {
			insertQueueFirst(extr);
		}
	}
	//este planificat noul thread din running
	ReleaseSemaphore(running->mutex_pl, 1, 0);
	//se asteapta ca thread-ul curent sa fie planificat
	WaitForSingleObject(thread->mutex_pl, INFINITE);
}


int so_init(unsigned int time_quantum, unsigned int io)
{
	ThreadInfo *ti;

	if (sch != NULL)
		return -1;
	sch = malloc(sizeof(scheduler));
	if (!sch)
		return -1;
	sch->time_quantum = time_quantum;
	if (time_quantum == 0 || io > SO_MAX_NUM_EVENTS) {
		free(sch);
		sch = NULL;
		return -1;
	}
	sch->io = io;

	ti = malloc(sizeof(ThreadInfo));
	if (ti == NULL) {
		free(sch);
		sch = NULL;
	}
	ti->priority = 0;
	ti->tid = GetCurrentThread();
	ti->urm = NULL;
	ti->time_quantum = sch->time_quantum;
	ti->mutex_pl = CreateSemaphore(0, 0, 1, 0);

	running = ti;

	myThread = TlsAlloc();
	TlsSetValue(myThread, ti);

	readyQueue = calloc(1 + SO_MAX_PRIO, sizeof(ThreadInfo *));
	blockingQueue = calloc(io, sizeof(ThreadInfo *));
	reschedule();
	return 0;
}

void destroy_thread(ThreadInfo *thread)
{
	CloseHandle(thread->mutex_pl);
	free(thread);
}

void destroy_scheduler(scheduler *sch)
{
	free(sch);
}

void so_end(void)
{
	ThreadInfo *thread, *t;

	if (sch == NULL)
		return;
	//last_thread asteapta ca READY sa se goleasca
	thread = TlsGetValue(myThread);
	last_thread = thread;

	running = extractQueue();
	if (running != NULL) {
		ReleaseSemaphore(running->mutex_pl, 1, 0);
		WaitForSingleObject(thread->mutex_pl, INFINITE);
	}

	t = terminated;
	while (t != NULL) {
		ThreadInfo *p = t;

		t = t->urm;
		WaitForSingleObject(p->tid, INFINITE);
		destroy_thread(p);
	}

	destroy_thread(thread);

	TlsFree(myThread);

	destroy_scheduler(sch);
	free(readyQueue);
	free(blockingQueue);
	sch = NULL;
}

void so_exec(void)
{
	ThreadInfo *thread = TlsGetValue(myThread);

	ReleaseSemaphore(thread->mutex_pl, 1, 0);
	WaitForSingleObject(thread->mutex_pl, INFINITE);
	reschedule();
}

int so_wait(unsigned int io)
{
	ThreadInfo *thread = TlsGetValue(myThread);

	ReleaseSemaphore(thread->mutex_pl, 1, 0);

	if (io < 0 || io >= sch->io)
		return -1;

	thread->urm = blockingQueue[io];
	blockingQueue[io] = thread;

	running = extractQueue();
	WaitForSingleObject(thread->mutex_pl, INFINITE);

	ReleaseSemaphore(running->mutex_pl, 1, 0);

	WaitForSingleObject(thread->mutex_pl, INFINITE);
	return 0;
}

DWORD start_thread(void *arg)
{
	//asteapta planificare
	param *p = (param *)arg;
	ThreadInfo *thread;

	TlsSetValue(myThread, p->t);
	thread = TlsGetValue(myThread);
	WaitForSingleObject(thread->mutex_pl, INFINITE);
	p->func(p->priority);

	free(p);

	running = extractQueue();
	thread->urm = terminated;
	terminated = thread;

	if (running != NULL) {
		ReleaseSemaphore(running->mutex_pl, 1, 0);
	} else {
		//se poate reactiva thread-ul care a apelat so_end
		ReleaseSemaphore(last_thread->mutex_pl, 1, 0);
	}
	return 0;
}

tid_t so_fork(so_handler *func, unsigned int priority)
{
	ThreadInfo *thread = TlsGetValue(myThread);
	ThreadInfo *t;
	param *p;
	DWORD tid;

	ReleaseSemaphore(thread->mutex_pl, 1, 0);
	if (func == NULL || priority > SO_MAX_PRIO)
		return INVALID_TID;
	t = malloc(sizeof(ThreadInfo));
	if (!t)
		return INVALID_TID;
	t->priority = priority;
	t->func = func;
	t->time_quantum = sch->time_quantum;
	t->mutex_pl = CreateSemaphore(0, 1, 1, 0);
	WaitForSingleObject(t->mutex_pl, INFINITE);

	p = malloc(sizeof(param));
	p->func = func;
	p->priority = priority;
	p->t = t;

	t->tid = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)start_thread,
			p, 0, &tid);

	//pun thread-ul in READY
	insertQueueLast(t);

	WaitForSingleObject(thread->mutex_pl, INFINITE);
	reschedule();
	return tid;
}

int so_signal(unsigned int io)
{
	ThreadInfo *thread = TlsGetValue(myThread);
	ThreadInfo *p;
	int nr = 0;

	ReleaseSemaphore(thread->mutex_pl, 1, 0);
	if (io < 0 || io >= sch->io)
		return -1;
	p = blockingQueue[io];
	while (p != NULL) {
		ThreadInfo *aux = p;

		p = p->urm;
		insertQueueLast(aux);
		nr++;
	}
	blockingQueue[io] = NULL;

	WaitForSingleObject(thread->mutex_pl, INFINITE);
	reschedule();
	return nr;
}

