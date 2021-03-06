#include <linux/vmalloc.h>
#include <linux/slab.h>

#include "flow.h"
#include "params.h"

//Print a flow information
//Type: Add(0) Delete(1)
void Karuna_Print_Flow(struct Karuna_Flow* f, int type)
{		
	char local_ip[16]={0};           	//Local IP address 
	char remote_ip[16]={0};		    //Remote IP address 
	
	snprintf(local_ip, 16, "%pI4", &(f->local_ip));
	snprintf(remote_ip, 16, "%pI4", &(f->remote_ip));
	
	if(type==0)
	{
		printk(KERN_INFO "Insert a Flow record: %s:%hu to %s:%hu \n",local_ip,f->local_port,remote_ip,f->remote_port);
	}
	else if(type==1)
	{
		printk(KERN_INFO "Delete a Flow record: %s:%hu to %s:%hu \n",local_ip,f->local_port,remote_ip,f->remote_port);
	}
	else
	{
		printk(KERN_INFO "Flow record: %s:%hu to %s:%hu \n",local_ip,f->local_port,remote_ip,f->remote_port);
	}
}

//Print a FlowNode
void Karuna_Print_Node(struct Karuna_Flow_Node* fn)
{		
	Karuna_Print_Flow(&(fn->f),2);
}

//Print a FlowList
void Karuna_Print_List(struct Karuna_Flow_List* fl)
{		
	struct Karuna_Flow_Node* Ptr;
	for(Ptr=fl->head->next;Ptr!=NULL;Ptr=Ptr->next)
	{
		Karuna_Print_Node(Ptr);
	}
}

//Print a FlowTable
void Karuna_Print_Table(struct Karuna_Flow_Table* ft)
{		
	int i=0;
	printk(KERN_INFO "Current flow table:\n");
	for(i=0;i<Karuna_HASH_RANGE;i++)
    {
		if(ft->table[i].len>0)
        {
			printk(KERN_INFO "FlowList %d\n",i);
			Karuna_Print_List(&(ft->table[i]));
        }
    }
	printk(KERN_INFO "There are %u flows in total\n",ft->size);
}

//Hash function, calculate the flow should be inserted into which Karuna_Flow_List
static inline unsigned int Karuna_Hash(struct Karuna_Flow* f)
{		
	//return a value in [0,HASH_RANGE-1]
	return ((f->local_ip/(256*256*256)+1)*(f->remote_ip/(256*256*256)+1)*(f->local_port+1)*(f->remote_port+1))%Karuna_HASH_RANGE;
}

//Determine whether two Flows are equal 
//<local_ip, remote_ip, local_port, remote_port> determines a flow
static inline bool Karuna_Equal(struct Karuna_Flow* f1,struct Karuna_Flow* f2)
{
	return ((f1->local_ip==f2->local_ip)
	&&(f1->remote_ip==f2->remote_ip)
	&&(f1->local_port==f2->local_port)
	&&(f1->remote_port==f2->remote_port));		
}

//Initialize the info of a Flow
void Karuna_Init_Info(struct Karuna_Flow_Info* info)
{
	//We need to initialize 10 variables in total
	info->latest_update_time=ktime_set(0,0);
	info->latest_timeout_time=ktime_set(0,0);
	info->latest_timeout_seq=0;
	info->latest_seq=0;
	info->latest_ack=0;
	info->bytes_sent=0;
	info->bytes_total=0;
	info->timeouts=0;
	info->is_size_known=0;
	info->is_deadline_known=0;
}

//Initialize a Flow
void Karuna_Init_Flow(struct Karuna_Flow* f)
{
	f->local_ip=0;	
	f->remote_ip=0;
	f->local_port=0;
	f->remote_port=0;
	
	//Initialize the Info of this Flow
	Karuna_Init_Info(&(f->info));
}

//Initialize a FlowNode
void Karuna_Init_Node(struct Karuna_Flow_Node* fn)
{
	//Initialize next pointer as null
	fn->next=NULL;
	//Initialize a flow structure
	Karuna_Init_Flow(&(fn->f));
}

//Initialize a FlowList
void Karuna_Init_List(struct Karuna_Flow_List* fl)
{
	struct Karuna_Flow_Node* buf=NULL;
	//No node in current list
	fl->len=0;
	//We use vmalloc here since this function is ususally referenced when we inset the module
	buf=vmalloc(sizeof(struct  Karuna_Flow_Node));
	if(buf==NULL)
	{
		printk(KERN_INFO "Vmalloc error in Karuna_Init_List\n");
	}
	else
	{
		fl->head=buf;
		Karuna_Init_Node(fl->head);
	}
}

//Initialize a FlowTable
void Karuna_Init_Table(struct Karuna_Flow_Table* ft)
{
	int i=0;
	struct Karuna_Flow_List* buf=NULL;
	
	//allocate space for FlowLists
	buf=vmalloc(Karuna_HASH_RANGE*sizeof(struct Karuna_Flow_List));
	if(buf==NULL)
	{
		printk(KERN_INFO "Vmalloc error in Karuna_Init_Table\n");
	}
	else
	{
		ft->table=buf;
		//Initialize each FlowList
		for(i=0;i<Karuna_HASH_RANGE;i++)
		{
			Karuna_Init_List(&(ft->table[i]));
		}
	}
	//No nodes in current table
	ft->size=0;
	//Init spinlock
	spin_lock_init(&(ft->tableLock));
}

//Insert a Flow into a FlowList and return 1 if it succeeds
unsigned int Karuna_Insert_List(struct Karuna_Flow_List* fl, struct Karuna_Flow* f, int flags)
{
	if(fl->len>=Karuna_LIST_SIZE) 
	{
		printk(KERN_INFO "Karuna_Insert_List: No enough space in this link list\n");
		return 0;
	} 
	else 
	{
        struct Karuna_Flow_Node* tmp=fl->head;
		struct Karuna_Flow_Node* buf=NULL;

        //Come to the tail of this FlowList
        while(1)
        {
			//If pointer to next node is NULL, we find the tail of this FlowList. Here we can insert our new Flow
            if(tmp->next==NULL)
            {
				//Allocate memory. The 'flags' is determined by context
				buf=kmalloc(sizeof(struct Karuna_Flow_Node),flags);
				if(buf==NULL) //Fail to allocate memory
				{
					printk(KERN_INFO "Karuna_Insert_List: Kmalloc error\n");
					return 0;
				}
				else
				{
					tmp->next=buf;
					//Copy data for this new FlowNode
					tmp->next->f=*f;
					//Pointer to next FlowNode is NUll
					tmp->next->next=NULL;
					//Increase length of FlowList
					fl->len++;
					return 1;
				}
			}
            //If the rule of next node is the same as our inserted flow, we just finish the insert  
			else if(Karuna_Equal(&(tmp->next->f),f)==1) 
			{
				printk(KERN_INFO "Karuna_Insert_List: Equal Flow\n");
				return 0;
			}
            else //Move to next FlowNode
            {
				tmp=tmp->next;
            }
       }
		return 0;
	}
}

//Insert a flow to FlowTable and return 1 if it succeeds
unsigned int Karuna_Insert_Table(struct Karuna_Flow_Table* ft,struct Karuna_Flow* f, int flags)
{
	unsigned int result=0;
	unsigned int index=Karuna_Hash(f);
	
	//printk(KERN_INFO "Insert to link list %d\n",index);
	//Insert Flow to appropriate FlowList based on Hash value
	result=Karuna_Insert_List(&(ft->table[index]),f,flags);
	//Increase the size of FlowTable
	ft->size+=result;
	
	return result;
}

//Search and return the pointer of information for a given flow in a FlowList
struct Karuna_Flow_Info* Karuna_Search_List(struct Karuna_Flow_List* fl, struct Karuna_Flow* f)
{
	//The length of FlowList is 0
	if(fl->len==0) 
	{
		return NULL;
	} 
	else 
	{
		struct Karuna_Flow_Node* tmp=fl->head;
		//Find the Flow in this FlowList
		while(1)
		{
			//If pointer to next node is NULL, we find the tail of this FlowList, no more FlowNodes to search
			if(tmp->next==NULL)
			{
				return NULL;
			}
			//Find matching flow (matching FlowNode is tmp->next rather than tmp)
			else if(Karuna_Equal(&(tmp->next->f),f)==1)
			{
				//return the info of this Flow
				return &(tmp->next->f.info);
			}	
			else
			{
				//Move to next FlowNode
				tmp=tmp->next;
			}
		}
	}
	return NULL;
}

//Search the information for a given Flow in a FlowTable
struct Karuna_Flow_Info* Karuna_Search_Table(struct Karuna_Flow_Table* ft, struct Karuna_Flow* f)
{
	unsigned int index=0;
	index=Karuna_Hash(f);
	return Karuna_Search_List(&(ft->table[index]),f);
}

//Delete a Flow from FlowList and return the bytes_sent/bytes_total/is_deadline_known of this flow if it succeeds
//If it is a deadline flow (type 0), return 1
//If it is a non-deadline flow but we know the size (type 1), return bytes_total (>=2)
//If it is a non-deadline flow and we don't know the size (type 2), return bytes_sent (>=2)
//This function should also set flow type 
u32 Karuna_Delete_List(struct Karuna_Flow_List* fl, struct Karuna_Flow* f, u8* type)
{
	u32 result=0;
	//No node in current FlowList
	if(fl->len==0) 
	{
		//printk(KERN_INFO "No node in current list\n");
		return 0;
	}
	else 
	{
		struct Karuna_Flow_Node* tmp=fl->head;
		struct Karuna_Flow_Node* s=NULL;

		while(1)	
		{
			//If pointer to next node is NULL, we find the tail of this FlowList, no more FlowNodes, return 0
			if(tmp->next==NULL) 
			{
				//printk(KERN_INFO "There are %d flows in this list\n",fl->len);
				return 0;
			}
			//Find the matching flow (matching FlowNode is tmp->next rather than tmp), delete flow and return
			else if(Karuna_Equal(&(tmp->next->f),f)==1) 
			{
				//If it's a deadline flow (type 0), return 1
				if(tmp->next->f.info.is_deadline_known)
				{
					result=1;
					*type=0;
				}
				//A non-deadline flow with priori knowledge of flow size (type 1), return bytes_total (>=2)
				else if(tmp->next->f.info.is_size_known)
				{
					result=max(2,tmp->next->f.info.bytes_total);
					*type=1;
				}
				//A non-deadline flow without priori knowledge of flow size (type 2), return bytes_sent (>=2)
				else
				{
					result=max(2,tmp->next->f.info.bytes_sent);
					*type=2;
				}
				 s=tmp->next;
				//Print_Flow(&(tmp->next->f),2);
				tmp->next=s->next;
				//Delete matching FlowNode from this FlowList
				kfree(s);
				//Reduce the length of this FlowList by one
				fl->len--;
				//printk(KERN_INFO "Delete a flow record\n");
				return result;
			}
			//Unmatch
			else 
			{
				//Move to next FlowNode
				tmp=tmp->next;
			}
		}
		return 0;
	}
}

//Delete a Flow from FlowTable and return corresponding result (see Karuna_Delete_List function) of this flow if it succeeds
u32 Karuna_Delete_Table(struct Karuna_Flow_Table* ft,struct Karuna_Flow* f, u8* type)
{
	u32 result=0;
	unsigned int index=0;
	index=Karuna_Hash(f);
	//printk(KERN_INFO "Delete from link list %d\n",index);
	//Delete Flow from appropriate FlowList based on Hash value
	result=Karuna_Delete_List(&(ft->table[index]),f, type);
	//Reduce the size of FlowTable by one
	if(result>0)
		ft->size-=1;
	//printk(KERN_INFO "Delete %d \n",result);
	return result;
}

void Karuna_Empty_List(struct Karuna_Flow_List* fl)
{
	struct Karuna_Flow_Node* NextNode;
	struct Karuna_Flow_Node* Ptr;
	for(Ptr=fl->head;Ptr!=NULL;Ptr=NextNode)
	{
		NextNode=Ptr->next;
		//Actually, we delete the fl->head in the first iteration
		//For fl->head, we use vfree. For other nodes, we use kfree
		if(Ptr==fl->head)
			vfree(Ptr);
		else
			kfree(Ptr);
	}
}

void Karuna_Empty_Table(struct Karuna_Flow_Table* ft)
{
	int i=0;
	for(i=0;i<Karuna_HASH_RANGE;i++)
	{
		Karuna_Empty_List(&(ft->table[i]));
	}
	vfree(ft->table);
}

