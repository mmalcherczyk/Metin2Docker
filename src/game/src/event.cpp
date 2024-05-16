/*
 *    Filename: event.c
 * Description: �̺�Ʈ ���� (timed event)
 *
 *      Author: ������ (aka. ��, Cronan), �ۿ��� (aka. myevan, ���ڷ�)
 */
#include "stdafx.h"

#include "event_queue.h"

extern void ContinueOnFatalError();
extern void ShutdownOnFatalError();

#ifdef M2_USE_POOL
MemoryPool event_info_data::pool_;
static ObjectPool<EVENT> event_pool;
#endif

static CEventQueue cxx_q;

/* �̺�Ʈ�� �����ϰ� �����Ѵ� */
LPEVENT event_create_ex(TEVENTFUNC func, event_info_data* info, int when)
{
	LPEVENT new_event = NULL;

	/* �ݵ�� ���� pulse �̻��� �ð��� ���� �Ŀ� �θ����� �Ѵ�. */
	if (when < 1)
		when = 1;

#ifdef M2_USE_POOL
	new_event = event_pool.Construct();
#else
	new_event = M2_NEW event;
#endif

	assert(NULL != new_event);

	new_event->func = func;
	new_event->info	= info;
	new_event->q_el	= cxx_q.Enqueue(new_event, when, thecore_heart->pulse);
	new_event->is_processing = false;
	new_event->is_force_to_end = false;

	return (new_event);
}

/* �ý������� ���� �̺�Ʈ�� �����Ѵ� */
void event_cancel(LPEVENT * ppevent)
{
	LPEVENT event;

	if (!ppevent)
	{
		SPDLOG_ERROR("null pointer");
		return;
	}

	if (!(event = *ppevent))
		return;

	if (event->is_processing)
	{
		event->is_force_to_end = true;

		if (event->q_el)
			event->q_el->bCancel = true;

		*ppevent = NULL;
		return;
	}

	// �̹� ��� �Ǿ��°�?
	if (!event->q_el)
	{
		*ppevent = NULL;
		return;
	}

	if (event->q_el->bCancel)
	{
		*ppevent = NULL;
		return;
	}

	event->q_el->bCancel = true;

	*ppevent = NULL;
}

void event_reset_time(LPEVENT event, int when)
{
	if (!event->is_processing)
	{
		if (event->q_el)
			event->q_el->bCancel = true;

		event->q_el = cxx_q.Enqueue(event, when, thecore_heart->pulse);
	}
}

/* �̺�Ʈ�� ������ �ð��� ������ �̺�Ʈ���� �����Ѵ� */
int event_process(int pulse)
{
	int	new_time;
	int		num_events = 0;

	// event_q �� �̺�Ʈ ť�� ����� �ð����� ������ pulse �� ������ �������� 
	// ���� �ʰ� �ȴ�.
	while (pulse >= cxx_q.GetTopKey())
	{
		TQueueElement * pElem = cxx_q.Dequeue();

		if (pElem->bCancel)
		{
			cxx_q.Delete(pElem);
			continue;
		}

		new_time = pElem->iKey;

		LPEVENT the_event = pElem->pvData;
		int processing_time = event_processing_time(the_event);
		cxx_q.Delete(pElem);

		/*
		 * ���� ���� ���ο� �ð��̸� ���� ���� 0 ���� Ŭ ��� �̺�Ʈ�� �ٽ� �߰��Ѵ�. 
		 * ���� ���� 0 �̻����� �� ��� event �� �Ҵ�� �޸� ������ �������� �ʵ���
		 * �����Ѵ�.
		 */
		the_event->is_processing = true;

		if (!the_event->info)
		{
			the_event->q_el = NULL;
			ContinueOnFatalError();
		}
		else
		{
			new_time = (the_event->func) (get_pointer(the_event), processing_time);
			
			if (new_time <= 0 || the_event->is_force_to_end)
			{
				the_event->q_el = NULL;
			}
			else
			{
				the_event->q_el = cxx_q.Enqueue(the_event, new_time, pulse);
				the_event->is_processing = false;
			}
		}

		++num_events;
	}

	return num_events;
}

/* �̺�Ʈ�� ����ð��� pulse ������ ������ �ش� */
int event_processing_time(LPEVENT event)
{
	int start_time;

	if (!event->q_el)
		return 0;

	start_time = event->q_el->iStartTime;
	return (thecore_heart->pulse - start_time);
}

/* �̺�Ʈ�� ���� �ð��� pulse ������ ������ �ش� */
int event_time(LPEVENT event)
{
	int when;

	if (!event->q_el)
		return 0;

	when = event->q_el->iKey;
	return (when - thecore_heart->pulse);
}

/* ��� �̺�Ʈ�� �����Ѵ� */
void event_destroy(void)
{
	TQueueElement * pElem;

	while ((pElem = cxx_q.Dequeue()))
	{
		LPEVENT the_event = (LPEVENT) pElem->pvData;

		if (!pElem->bCancel)
		{
			// no op here
		}

		cxx_q.Delete(pElem);
	}
}

int event_count()
{
	return cxx_q.Size();
}

void intrusive_ptr_add_ref(EVENT* p) {
	++(p->ref_count);
}

void intrusive_ptr_release(EVENT* p) {
	if ( --(p->ref_count) == 0 ) {
#ifdef M2_USE_POOL
		event_pool.Destroy(p);
#else
		M2_DELETE(p);
#endif
	}
}
