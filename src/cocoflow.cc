#include "cocoflow-comm.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include "max_map_count.h"
#endif

#define RESV_MAP_COUNT 5530

namespace ccf {

struct setting
{
	uint32 stack_size;
	uint32 protect_size;
	uint32 max_task_num;
	setting(uint32 stack_size, uint32 protect_size, uint32 max_task_num)
		: stack_size(stack_size), protect_size(protect_size), max_task_num(max_task_num)
	{}
};

/* extern in cocoflow.h */
bool          global_initialized = false;
event_task**  global_task_manager = NULL;

/* extern in cocoflow-comm.h */
coroutine*    global_running_manager = NULL;
coroutine     global_loop_running;
event_task*   global_current_task = NULL;
bool          global_signal_canceled = false;
FILE*         global_debug_file = NULL;
char          global_debug_output_src[CLASS_TIPS_MAX_LEN];
char          global_debug_output_dst[CLASS_TIPS_MAX_LEN];

static std::list<setting> setting_list;
static uint32 g_max_task_num = 0;
static size_t g_max_stack_size = 0;
static event_task* top_task;
static int g_max_map_count = 0;

void set_debug(FILE* fp)
{
	global_debug_file = fp;
}

inline void __task_start_child(event_task* cur, event_task* child)
{
	swap_running(cur->_unique_id, child->_unique_id);
}

inline void __task_cancel_children(event_task* cur, event_task** children, uint32 num)
{
	uint32 unsupport = 0;
	for (uint32 i=0; i<num; i++)
	{
		event_task* child = children[i];
		if (task_get_status(child) == running)
		{
			for (event_task* final = child; ; final = final->reuse)
			{
				if (task_is_uninterruptable(final))
				{
					if (final != child)
						task_set_status(final, canceled);
					unsupport ++;
					if (ccf_unlikely(global_debug_file))
						LOG_DEBUG("[Logic] [any_of]  %u-<%s> is going to cancel %u-<%s> but uninterruptable",
							cur->_unique_id, src_to_tips(cur), child->_unique_id, dst_to_tips(child));
					break;
				}
				if (!final->reuse)
				{
					global_signal_canceled = true;
					task_set_status(child, canceled);
					if (ccf_unlikely(global_debug_file))
						LOG_DEBUG("[Logic] [any_of]  %u-<%s> is going to cancel %u-<%s>",
							cur->_unique_id, src_to_tips(cur), child->_unique_id, dst_to_tips(child));
					swap_running(cur->_unique_id, child->_unique_id);
					break;
				}
			}
		}
	}
	if (unsupport)
	{
		cur->uninterruptable();
		while (unsupport > 0)
		{
			if (ccf_unlikely(global_debug_file))
				LOG_DEBUG("[Logic] [any_of]  %u-<%s> is waiting for all of %u uninterruptable task%s",
					cur->_unique_id, src_to_tips(cur), unsupport, unsupport>1? "s": "");
			CHECK(__task_yield(cur) == true);
			unsupport --;
		}
		if (ccf_unlikely(global_debug_file))
			LOG_DEBUG("[Logic] [any_of]  %u-<%s> completed its goal for uninterruptable tasks", cur->_unique_id, src_to_tips(cur));
	}
}

void __task_runtime(uint32);

void __init_setting(uint32* &free_list_front, uint32* &free_list_end, uint32 stack_size, uint32 protect_size, uint32 max_task_num)
{
	free_list_front = new uint32[max_task_num];
	free_list_end = free_list_front + max_task_num;
	for (uint32 i=0; i<max_task_num; i++, g_max_task_num++)
		free_list_front[i] = g_max_task_num;
	g_max_stack_size += stack_size * (size_t)max_task_num;
	setting_list.push_back(setting(stack_size, protect_size, max_task_num));
	if (protect_size)
		g_max_map_count += 2 * max_task_num;
}

void __init()
{
	global_initialized = true;
	
#if !defined(_WIN32) && !defined(_WIN64)
	int max_map_count = get_max_map_count();
	if (max_map_count > RESV_MAP_COUNT && g_max_map_count > max_map_count - RESV_MAP_COUNT)
		FATAL_ERROR("Out of memory (max-map-count:%d(resv:%d), required-map-count:%d)", max_map_count, RESV_MAP_COUNT, g_max_map_count);
#endif
	
	void* mem = coroutine_memory_alloc(g_max_stack_size);
	if (!mem)
		FATAL_ERROR("Out of memory");
	
	global_task_manager = new event_task*[g_max_task_num];
	global_running_manager = new coroutine[g_max_task_num];
	
	uint32 unique_id = 0;
	void* cur_mem = mem;
	for (std::list<setting>::iterator it = setting_list.begin(); it != setting_list.end(); it++)
	{
		for (uint32 i=0; i<it->max_task_num; i++)
		{
			if (it->protect_size)
				coroutine_memory_protect(cur_mem, it->protect_size);
			coroutine_create(&global_running_manager[unique_id], cur_mem, it->stack_size, unique_id);
			unique_id ++;
			cur_mem = (void*)((char*)cur_mem + it->stack_size);
		}
	}
}

void __task_runtime(uint32 _unique_id)
{
	for (;;)
	{
		event_task* this_task = global_task_manager[_unique_id];
		
		task_set_status(this_task, running);
		try {
			this_task->run();
		} catch (interrupt_canceled&) {
			task_set_status(this_task, canceled);
			this_task->cancel();
		}
		if (task_get_status(this_task) == running)
			task_set_status(this_task, completed);
			
		uint32 next;
		if (this_task->finish_to != EVENT_LOOP_ID || this_task->block_to == EVENT_LOOP_ID)
			next = this_task->finish_to; //End of await or start
		else
			next = this_task->block_to; //End of start without really block
		
		if (ccf_unlikely(global_debug_file) && task_is_all_any(this_task))
		{
			event_task* parent = global_task_manager[this_task->finish_to];
			while (parent->reuse)
				parent = parent->reuse;
			if (dynamic_cast<any_of*>(parent))
				LOG_DEBUG("[Logic] [any_of]  %u-<%s> is %s",
					_unique_id, dst_to_tips(this_task), task_get_status(this_task) != canceled? "completed": "canceled");
			else if (dynamic_cast<all_of*>(parent))
				LOG_DEBUG("[Logic] [all_of]  %u-<%s> is %s",
					_unique_id, dst_to_tips(this_task), task_get_status(this_task) != canceled? "completed": "canceled");
			else
				CHECK(0);
		}
			
		if (this_task->finish_to == EVENT_LOOP_ID && this_task != top_task)
		{
			global_task_manager[_unique_id] = NULL;
			if (ccf_unlikely(global_debug_file))
			{
				if (next == EVENT_LOOP_ID)
					LOG_DEBUG("[Logic] [start]   %u-<%s> is completed", _unique_id, dst_to_tips(this_task));
				else
					LOG_DEBUG("[Logic] [start]   %u-<%s> is completed without block", _unique_id, dst_to_tips(this_task));
			}
			delete this_task;
		}
		
		swap_running(_unique_id, next);
	}
}

int __await0(event_task* target)
{
	if (ccf_unlikely(!global_current_task))
		FATAL_ERROR("Call await() must be in a task");
	
	if (ccf_unlikely(task_get_status(target) != ready && task_get_status(target) != limited))
		return -1;
	
	event_task* parent = global_current_task;
	
	//Passing parent's block to child's block
	target->block_to = parent->block_to;
	parent->block_to = EVENT_LOOP_ID;
	
	target->finish_to = parent->_unique_id;
	target->_unique_id = parent->_unique_id;
	
	task_set_status(target, running);
	global_current_task = target;
	parent->reuse = target;
	
	if (ccf_unlikely(global_debug_file))
		LOG_DEBUG("[Logic] [await]   %u-<%s> is waiting for %u-<%s>",
			parent->_unique_id, src_to_tips(parent), target->_unique_id, dst_to_tips(target));
	
	try {
		target->run();
	} catch (interrupt_canceled& sig) {
		task_set_status(target, canceled);
		target->cancel();
		
		if (ccf_unlikely(global_debug_file))
			LOG_DEBUG("[Logic] [await]   %u-<%s> is interrupted", target->_unique_id, dst_to_tips(target));
		
		//Must be careful
		global_current_task = parent;
		parent->reuse = NULL;
		target->_unique_id = EVENT_LOOP_ID;
		
		throw interrupt_canceled(sig.level + 1);
	}
	
	if (ccf_unlikely(global_debug_file))
	{
		if (task_get_status(target) != canceled)
			LOG_DEBUG("[Logic] [await]   %u-<%s> is completed (maybe without block)", target->_unique_id, dst_to_tips(target));
		else
			LOG_DEBUG("[Logic] [await]   %u-<%s> is completed with canceled-signal", target->_unique_id, dst_to_tips(target));
	}
	
	global_current_task = parent;
	parent->reuse = NULL;
	if (task_get_status(target) == running)
		task_set_status(target, completed);
		
	target->_unique_id = EVENT_LOOP_ID;
	
	//Passing child's block to parent's block cause child without really block
	if (target->block_to != EVENT_LOOP_ID)
	{
		parent->block_to = target->block_to;
		target->block_to = EVENT_LOOP_ID;
	}
	
	if (task_get_status(target) == canceled) //Only unsupport cancel can reach
	{
		task_set_status(target, completed);
		throw interrupt_canceled(0);
	}
	
	return 0;
}

int __start(event_task* target)
{
	if (ccf_unlikely(!global_current_task))
		FATAL_ERROR("Call start() must be in a task");
	
	if (ccf_unlikely(task_get_status(target) != ready))
	{
		delete target;
		return -1;
	}
	
	event_task* parent = global_current_task;
	
	target->block_to = parent->_unique_id;
	target->finish_to = EVENT_LOOP_ID;
	
	if (ccf_unlikely(global_debug_file))
		LOG_DEBUG("[Logic] [start]   %u-<%s> is going to start %u-<%s>",
			parent->_unique_id, src_to_tips(parent), target->_unique_id, dst_to_tips(target));
	
	swap_running(parent->_unique_id, target->_unique_id);
	
	return 0;
}

void __cocoflow(event_task* top)
{
	static int call = 0;
	CHECK((++call) == 1);
	
	top_task = top;
	
	top_task->block_to = EVENT_LOOP_ID;
	top_task->finish_to = EVENT_LOOP_ID;
	
	if (ccf_unlikely(global_debug_file))
		LOG_DEBUG("[Logic] cocoflow is beginning at %u-<%s>", top_task->_unique_id, dst_to_tips(top_task));
	
	coroutine_by_thread(&global_loop_running);
	swap_running(EVENT_LOOP_ID, top_task->_unique_id);
	
	(void)uv_run(loop(), UV_RUN_DEFAULT);
	
	if (ccf_unlikely(global_debug_file))
		LOG_DEBUG("[Logic] cocoflow is all completed");
}

/***** all_of *****/

all_of::all_of(event_task* targets[], uint32 num) : num(num)
{
	CHECK(this->num != 0);
	this->children = targets;
}

void all_of::run()
{
	uint32 count = this->num;
	
	for (uint32 i=0; i<this->num; i++)
	{
		if (ccf_unlikely(task_get_status(this->children[i]) != ready))
		{
			task_set_status(this, child_unready);
			return;
		}
		this->children[i]->block_to = this->_unique_id;
		this->children[i]->finish_to = this->_unique_id;
	}
	
	for (uint32 i=0; i<this->num; i++)
	{
		if (ccf_unlikely(global_debug_file))
			LOG_DEBUG("[Logic] [all_of]  %u-<%s> is going to start %u-<%s>",
				this->_unique_id, src_to_tips(this), this->children[i]->_unique_id, dst_to_tips(this->children[i]));
		__task_start_child(this, this->children[i]);
		if (task_get_status(this->children[i]) == completed)
		{
			count --;
			if (ccf_unlikely(global_debug_file))
				LOG_DEBUG("[Logic] [all_of]  %u-<%s> is completed without block",
					this->children[i]->_unique_id, dst_to_tips(this->children[i]));
		}
		else
			task_set_all_any(this->children[i]);
	}
	
	while (count > 0)
	{
		if (ccf_unlikely(global_debug_file))
			LOG_DEBUG("[Logic] [all_of]  %u-<%s> is waiting for all of %u task%s",
				this->_unique_id, src_to_tips(this), count, count>1? "s": "");
		if (!__task_yield(this))
			return;
		count --;
	}
	
	if (ccf_unlikely(global_debug_file))
		LOG_DEBUG("[Logic] [all_of]  %u-<%s> completed its goal", this->_unique_id, src_to_tips(this));
}

void all_of::cancel()
{
	__task_cancel_children(this, this->children, this->num);
}

all_of::~all_of()
{
}

/***** any_of *****/

any_of::any_of(event_task* targets[], uint32 num) : num(num), completed_id(-1)
{
	CHECK(this->num != 0);
	this->children = targets;
}

int any_of::who_completed()
{
	return this->completed_id;
}

void any_of::run()
{
	for (uint32 i=0; i<this->num; i++)
	{
		if (ccf_unlikely(task_get_status(this->children[i]) != ready))
		{
			task_set_status(this, child_unready);
			return;
		}
		this->children[i]->block_to = this->_unique_id;
		this->children[i]->finish_to = this->_unique_id;
	}
	
	for (uint32 i=0; i<this->num; i++)
	{
		if (ccf_unlikely(global_debug_file))
			LOG_DEBUG("[Logic] [any_of]  %u-<%s> is going to start %u-<%s>",
				this->_unique_id, src_to_tips(this), this->children[i]->_unique_id, dst_to_tips(this->children[i]));
		__task_start_child(this, this->children[i]);
		if (task_get_status(this->children[i]) == completed)
		{
			this->completed_id = i;
			if (ccf_unlikely(global_debug_file))
				LOG_DEBUG("[Logic] [any_of]  %u-<%s> is completed without block",
					this->children[i]->_unique_id, dst_to_tips(this->children[i]));
			break;
		}
		else
			task_set_all_any(this->children[i]);
	}
	
	if (this->completed_id == -1)
	{
		if (ccf_unlikely(global_debug_file))
			LOG_DEBUG("[Logic] [any_of]  %u-<%s> is waiting for any of %u task%s",
				this->_unique_id, src_to_tips(this), this->num, this->num>1? "s": "");
		if (!__task_yield(this))
			return;
		for (uint32 i=0; i<this->num; i++)
			if (task_get_status(this->children[i]) == completed)
			{
				this->completed_id = i;
				break;
			}
	}
	
	if (ccf_unlikely(global_debug_file))
		LOG_DEBUG("[Logic] [any_of]  %u-<%s> completed its goal", this->_unique_id, src_to_tips(this));
	
	__task_cancel_children(this, this->children, this->num);
}

void any_of::cancel()
{
	__task_cancel_children(this, this->children, this->num);
}

any_of::~any_of()
{
}

/***** uv *****/

void free_self_close_cb(uv_handle_t* handle)
{
	free(handle);
}

}
