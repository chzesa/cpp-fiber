#include "fiber.hpp"

namespace czsfiber
{

#define PUSHA	"pushq %%rbx \n\t"	\
		"pushq %%r12 \n\t"	\
		"pushq %%r13 \n\t"	\
		"pushq %%r14 \n\t"	\
		"pushq %%r15 \n\t"

#define POPA	"popq %%r15 \n\t"	\
		"popq %%r14 \n\t"	\
		"popq %%r13 \n\t"	\
		"popq %%r12 \n\t"	\
		"popq %%rbx"

static const uint64_t STACK_SIZE = 1024 * 128;

static std::atomic_flag QUEUE_LOCK = ATOMIC_FLAG_INIT;
static Fiber* HEAD = nullptr;
static Fiber* TAIL = nullptr;

static thread_local std::atomic_flag* HELD_LOCK = nullptr;
static thread_local Fiber* EXEC_FIBER = nullptr;
static thread_local uint64_t P_BASE = 0;
static thread_local uint64_t P_STACK = 0;

enum FiberStatus
{
	New,
	Active,
	Blocked,
	Done
};

enum YieldType
{
	Acquire,
	Block,
	Return
};

void yield(YieldType ty);
void yield() { yield(YieldType::Acquire); }

struct Fiber
{
	char m_stack[STACK_SIZE];

	uint64_t m_fiberStack;
	uint64_t m_fiberBase;

	void (*m_task)(void*);
	void* m_param;

	Sync* m_signal;
	FiberStatus m_status;
	Fiber* next;
};


void acquireLock()
{
	while(QUEUE_LOCK.test_and_set(std::memory_order_relaxed));
}

void releaseLock()
{
	QUEUE_LOCK.clear();
}

TaskDecl::TaskDecl()
{
	m_task = nullptr;
	m_param = nullptr;
}

void append(Fiber* head, Fiber* tail)
{
	tail->next = nullptr;
	acquireLock();
	if (TAIL == nullptr)
	{
		HEAD = head;
		TAIL = tail;
	}
	else
	{
		TAIL->next = head;
		TAIL = tail;
	}
	releaseLock();
}

void Barrier::signal()
{
	while(m_lock.test_and_set(std::memory_order_relaxed));
	if (m_value > 0 && --m_value == 0)
	{
		Fiber* head = m_head;
		Fiber* tail = m_tail;
		m_head = nullptr;
		m_tail = nullptr;
		m_lock.clear();

		if (head == nullptr)
		{
			return;
		}

		append(head, tail);
	} else {
		m_lock.clear();
	}
}

void Barrier::wait()
{
	Fiber* fiber = EXEC_FIBER;
	while(m_lock.test_and_set(std::memory_order_relaxed));

	if (m_value == 0)
	{
		m_lock.clear();
		return;
	}

	fiber->m_status = FiberStatus::Blocked;

	if (m_tail == nullptr)
	{
		m_head = fiber;
		m_tail = fiber;
	}
	else
	{
		m_tail->next = fiber;
		m_tail = fiber;
	}

	HELD_LOCK = &m_lock;
	yield(YieldType::Block);
}

void Semaphore::signal()
{
	while(m_lock.test_and_set(std::memory_order_relaxed));

	if (m_head != nullptr)
	{
		Fiber* head = m_head;
		Fiber* tail = m_tail;

		if (m_head == m_tail)
		{
			m_head = nullptr;
			m_tail = nullptr;
		}
		else
		{
			m_head = head->next;
		}
		m_lock.clear();
		append(head, tail);
	} else {
		m_value++;
		m_lock.clear();
	}
}

void Semaphore::wait()
{
	while(m_lock.test_and_set(std::memory_order_relaxed));

	if (m_value > 0)
	{
		m_value--;
		m_lock.clear();
		return;
	}

	Fiber* fiber = EXEC_FIBER;
	fiber->m_status = FiberStatus::Blocked;

	if (m_tail == nullptr)
	{
		m_head = fiber;
		m_tail = fiber;
	}
	else
	{
		m_tail->next = fiber;
	}

	HELD_LOCK = &m_lock;
	yield(YieldType::Block);
}

Fiber* acquireNext()
{
	acquireLock();
	Fiber* fiber = HEAD;
	if (fiber != nullptr)
	{
		if (HEAD == TAIL)
		{
			HEAD = nullptr;
			TAIL = nullptr;
		}
		else
		{
			HEAD = fiber->next;
		}
	}
	releaseLock();
	return fiber;
}

void execFiber()
{
	Fiber* fiber = EXEC_FIBER;
	fiber->m_task(fiber->m_param);
	if (fiber->m_signal != nullptr)
	{
		fiber->m_signal->signal();
	}
	fiber->m_status = FiberStatus::Done;
	yield(YieldType::Return);
}

void __attribute__((noinline)) yield(YieldType ty)
{
	Fiber* fiber;
	uint64_t p_stack;
	uint64_t p_base;

	switch (ty)
	{
	case YieldType::Block:
		fiber = EXEC_FIBER;
		asm volatile
		(
			PUSHA
			"movq %%rsp, %0\n\t"
			"movq %%rbp, %1"
			:"=r" (p_stack)
			,"=r" (p_base)
		);

		fiber->m_fiberStack = p_stack;
		fiber->m_fiberBase = p_base;
		HELD_LOCK->clear();

	case YieldType::Return:
		p_stack = P_STACK;
		p_base = P_BASE;
		asm volatile
		(
			"movq %0, %%rsp\n\t"
			"movq %1, %%rbp\n\t"
			POPA
			:
			:"r" (p_stack)
			,"r" (p_base)
		);

		switch(EXEC_FIBER->m_status)
		{
		case FiberStatus::Done:
			delete EXEC_FIBER;
			break;
		case FiberStatus::Blocked:
			HELD_LOCK = nullptr;
			break;
		}

		EXEC_FIBER = nullptr;

	case YieldType::Acquire:
		fiber = acquireNext();
		if (fiber == nullptr)
		{
			return;
		}

		asm volatile
		(
			PUSHA
			"movq %%rsp, %0\n\t"
			"movq %%rbp, %1"
			:"=r" (p_stack)
			,"=r" (p_base)
		);

		P_STACK = p_stack;
		P_BASE = p_base;

		p_stack = fiber->m_fiberStack;
		p_base = fiber->m_fiberBase;

		EXEC_FIBER = fiber;

		switch (fiber->m_status)
		{
		case FiberStatus::New:
			fiber->m_status = FiberStatus::Active;

			asm volatile
			(
				"movq %0, %%rsp\n\t"
				"movq %1, %%rbp"
				:
				:"r" (p_stack)
				,"r" (p_base)
			);

			execFiber();
			break;

		case FiberStatus::Blocked:
			fiber->m_status = FiberStatus::Active;

			asm volatile
			(
				"movq %0, %%rsp\n\t"
				"movq %1, %%rbp\n\t"
				POPA
				:
				:"r" (p_stack)
				,"r" (p_base)
			);
			break;
		}

		break;
	}
}

void runTasks(TaskDecl* decl, uint64_t numTasks, Barrier** p_barrier)
{
	Barrier* barrier = nullptr;

	if (p_barrier != nullptr)
	{
		barrier = new Barrier;
		barrier->m_value = numTasks;
		(*p_barrier) = barrier;
	}

	Fiber* fibers[numTasks];

	for (uint64_t i = 0; i < numTasks; i++)
	{
		Fiber* fiber = new Fiber;

		fiber->m_fiberStack = (uint64_t) &(fiber->m_fiberStack);
		fiber->m_fiberBase = fiber->m_fiberStack;

		fiber->m_task = decl[i].m_task;
		fiber->m_param = decl[i].m_param;
		fiber->m_signal = barrier;
		fiber->m_status = FiberStatus::New;

		fibers[i] = fiber;
		if (i > 0)
		{
			fibers[i - 1]->next = fiber;
		}
	}

	append(fibers[0], fibers[numTasks - 1]);
}

}