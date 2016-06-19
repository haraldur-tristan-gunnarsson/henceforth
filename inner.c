#define _GNU_SOURCE 0xF00BAA //value unimportant, required for mremap
#include <stdio.h>
#include <stdlib.h>//exit
#include <stddef.h>//size_t etc.
#include <string.h>//strcpy etc.
#include <unistd.h>//sysconf
#include <sys/mman.h>//mmap, mremap
#include <limits.h>//__WORDSIZE
#include <errno.h>

//#define DEBUG
FILE *global_in = NULL;

void error_exit(char *message){
	puts(message);
	exit(-1);
}

struct entry{
	struct entry *next;
	char *name;
	union{
		size_t *threaded;
		void (*native)(void);
	} code;
	int can_delete;
	int code_size;//if zero, then native. negative invalid
};
#define NATIVE_CODE(CNAME) void CNAME##_native(void)
#define NATIVE_ENTRY(CNAME,NAME,NEXT) \
struct entry CNAME##_entry ={ \
	.next = &NEXT##_entry, \
	.name = NAME, \
	.code.native = &CNAME##_native, \
	.code_size = 0, \
	.can_delete = 0, \
};
#define THREADED_CODE(CNAME) size_t CNAME##_threaded[]
#define THREADED_ENTRY(CNAME,NAME,NEXT) \
struct entry CNAME##_entry ={ \
	.next = &NEXT##_entry, \
	.name = NAME, \
	.code.threaded = CNAME##_threaded, \
	.code_size = (sizeof CNAME##_threaded)/(sizeof(size_t*)), \
	.can_delete = 0, \
};

void null_entry_code(void){
	error_exit("******ERROR: executed null entry \"\" at end of dictionary");
}
struct entry null_entry ={
	.next = NULL,
	.name = "",
	.code.native = &null_entry_code,
	.code_size = 0,
	.can_delete = 0,
};

size_t *code_spc = NULL;
size_t *code_ptr = NULL;
size_t *code_end = NULL;
size_t page_size = 0;
size_t data_store_size = 0;
#define TEMP_ENTRY_MAX (0x20)
struct entry temp_entry_store[TEMP_ENTRY_MAX];
struct entry *temp_entry_ptr = temp_entry_store;
struct entry *temp_entry_head = NULL;
#define TEMP_ENTRY_NAME_LENGTH_MAX 0x100
char temp_entry_name_store[TEMP_ENTRY_MAX * TEMP_ENTRY_NAME_LENGTH_MAX];
char *temp_entry_name_ptr = temp_entry_name_store;
#define STACK_SIZE (0x10000)
size_t data_stack[STACK_SIZE];
size_t *data_ptr = data_stack;
size_t proc_stack[STACK_SIZE];
size_t *proc_ptr = proc_stack;
size_t *inst_ptr = NULL;

NATIVE_CODE(reset_stack){
	data_ptr = data_stack;
}  NATIVE_ENTRY(reset_stack,"reset_stack",null);

NATIVE_CODE(show_data){
	size_t *ii;
	printf("data: ");
	for(ii = data_stack; ii < data_ptr; ++ii)printf("%zd ", *ii);
	puts("");
} NATIVE_ENTRY(show_data,".s",reset_stack);

NATIVE_CODE(show_proc){
	size_t *ii;
	printf("proc: ");
	for(ii = proc_stack; ii < proc_ptr; ++ii)printf("%zd ", *ii);
	puts("");
} NATIVE_ENTRY(show_proc,".r",show_data);

void push_data(size_t value){
#ifdef DEBUG
	show_data_native();
#endif
	*data_ptr = value;
	data_ptr++;
	if(data_ptr>data_stack+STACK_SIZE)error_exit("data stack overflow");
#ifdef DEBUG
	show_data_native();
#endif
}
size_t pop_data(void){
#ifdef DEBUG
	show_data_native();
#endif
	data_ptr--;
	if(data_ptr<data_stack)error_exit("data stack underflow");
#ifdef DEBUG
	show_data_native();
#endif
	return *data_ptr;
}
size_t read_data(void){return *(data_ptr - 1);}

void push_proc(size_t value){
#ifdef DEBUG
	show_proc_native();
#endif
	*proc_ptr = value;
	proc_ptr++;
	if(proc_ptr>proc_stack+STACK_SIZE)error_exit("proc stack overflow");
#ifdef DEBUG
	show_proc_native();
#endif
}
size_t pop_proc(void){
#ifdef DEBUG
	show_proc_native();
#endif
	proc_ptr--;
	if(proc_ptr<proc_stack)error_exit("proc stack underflow");
#ifdef DEBUG
	show_proc_native();
#endif
	return *proc_ptr;
}
size_t read_proc(void){return *(proc_ptr - 1);}

NATIVE_CODE(ret){
#ifdef DEBUG
	show_data_native();
	show_proc_native();
	printf("%d %s\n", __LINE__, __FILE__);
	printf("inst_ptr: %p\n", inst_ptr);
#endif
	inst_ptr = (size_t*)pop_proc();
#ifdef DEBUG
	printf("inst_ptr: %p\n", inst_ptr);
#endif
} NATIVE_ENTRY(ret,"RET",show_proc);

NATIVE_CODE(cin){
	push_data((size_t)getc(global_in));
} NATIVE_ENTRY(cin,"CIN",ret);

NATIVE_CODE(pagesize){
	push_data(page_size);
} NATIVE_ENTRY(pagesize,"PAGESIZE",cin);

NATIVE_CODE(code_ptr){
	push_data((size_t)(&code_ptr));
} NATIVE_ENTRY(code_ptr,"CPTR",pagesize);

NATIVE_CODE(code_end){
	push_data((size_t)(&code_end));
} NATIVE_ENTRY(code_end,"CEND",code_ptr);

NATIVE_CODE(at){
	push_data(*((size_t *)pop_data()));
} NATIVE_ENTRY(at,"@",code_end);

NATIVE_CODE(set_value){
	size_t *ref = (size_t *)pop_data();
	size_t val = pop_data();
	*ref = val;
} NATIVE_ENTRY(set_value,"!",at);

NATIVE_CODE(char_at){
	push_data((size_t)(*((char *)pop_data())));
} NATIVE_ENTRY(char_at,"c@",set_value);

NATIVE_CODE(char_set_value){
	char *ref = (char *)pop_data();
	char val = (char)pop_data();
	*ref = val;
} NATIVE_ENTRY(char_set_value,"c!",char_at);

NATIVE_CODE(literal){
	push_data(*inst_ptr++);
} NATIVE_ENTRY(literal,"LITERAL",char_set_value);

NATIVE_CODE(m_set_end){
	size_t addr = pop_data();
	size_t temp = page_size - 1;
	addr += temp;
	addr &= ~temp;
	if(addr >= (size_t)code_spc + data_store_size)error_exit("data overflow: mremap");
	code_end = (size_t*)addr;
	size_t trimmed_size = addr - (size_t)code_spc;//beware pointer size
	//The Linux kernel's virtual memory manager is "lazy" --
	//it only allocates physical memory to a process once it is read from
	//or written to. Here, memory beyond the new address is freed from this
	//process and then the freed virtual memory address range is reclaimed,
	//to prevent the kernel from placing anything else within it, and,
	//by so doing, prevent this process from expanding into it.
	void *res = MAP_FAILED;
	res = mremap(code_spc, data_store_size, trimmed_size, 0);
	if(MAP_FAILED == res)error_exit("mremap failed for m_set_end (first time)");
	res = mremap(code_spc, trimmed_size, data_store_size, 0);
	if(MAP_FAILED == res)error_exit("mremap failed for m_set_end (second time)");
} NATIVE_ENTRY(m_set_end,"M_SET_END",literal);

NATIVE_CODE(drop){
	(void)pop_data();
} NATIVE_ENTRY(drop,"DROP",m_set_end);

NATIVE_CODE(rdrop){
	(void)pop_proc();
} NATIVE_ENTRY(rdrop,"RDROP",drop);

NATIVE_CODE(swap){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val2);
	push_data(val1);
} NATIVE_ENTRY(swap,"SWAP",rdrop);

NATIVE_CODE(s_to_r){
	push_proc(pop_data());
} NATIVE_ENTRY(s_to_r,">r",swap);

NATIVE_CODE(r_to_s){
	push_data(pop_proc());
} NATIVE_ENTRY(r_to_s,"r>",s_to_r);

NATIVE_CODE(s_to_r_copy){
	push_proc(read_data());
} NATIVE_ENTRY(s_to_r_copy,"@r",r_to_s);

NATIVE_CODE(r_to_s_copy){
	push_data(read_proc());
} NATIVE_ENTRY(r_to_s_copy,"r@",s_to_r_copy);

NATIVE_CODE(here){
	code_ptr_native();
	at_native();
} NATIVE_ENTRY(here,"HERE",r_to_s_copy);

NATIVE_CODE(dot){
	printf("%zd\n",pop_data());
} NATIVE_ENTRY(dot,".",here);

NATIVE_CODE(mul){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1*val2);
} NATIVE_ENTRY(mul,"*",dot);

NATIVE_CODE(div){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1/val2);
} NATIVE_ENTRY(div,"/",mul);

NATIVE_CODE(sub){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1-val2);
} NATIVE_ENTRY(sub,"-",div);

NATIVE_CODE(add){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1+val2);
} NATIVE_ENTRY(add,"+",sub);

NATIVE_CODE(not){
	push_data(pop_data() ? 0 : ~0);// -1 or ~0 is TRUE, 0 is FALSE
} NATIVE_ENTRY(not, "NOT", add);

NATIVE_CODE(bnot){
	push_data(~pop_data());
} NATIVE_ENTRY(bnot,"BNOT",not);

NATIVE_CODE(bor){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1|val2);
} NATIVE_ENTRY(bor,"BOR",bnot);

NATIVE_CODE(band){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1&val2);
} NATIVE_ENTRY(band,"BAND",bor);

NATIVE_CODE(gt){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1<=val2);
	not_native();//In C, TRUE is 1, while I want TRUE to be -1
} NATIVE_ENTRY(gt,">",band);

NATIVE_CODE(lt){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1>=val2);
	not_native();//In C, TRUE is 1, while I want TRUE to be -1
} NATIVE_ENTRY(lt,"<",gt);

NATIVE_CODE(ge){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1<val2);
	not_native();//In C, TRUE is 1, while I want TRUE to be -1
} NATIVE_ENTRY(ge,">=",lt);

NATIVE_CODE(le){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1>val2);
	not_native();//In C, TRUE is 1, while I want TRUE to be -1
} NATIVE_ENTRY(le,"<=",ge);

THREADED_CODE(cell) = {
	(size_t)&literal_entry,
	sizeof data_ptr,
	(size_t)&ret_entry,
}; THREADED_ENTRY(cell,"CELL",le);

NATIVE_CODE(call){
	struct entry *xt = (struct entry *)pop_data();
#ifdef DEBUG
	printf("inst_ptr: %p after %s (%p->%p)\n", inst_ptr, xt->name, xt, (void*)xt->code.threaded);
	if(!xt->code_size)puts("native");
#endif
	if(xt->code_size){
		push_proc((size_t)inst_ptr);
		inst_ptr = xt->code.threaded;
	}
	else{
		xt->code.native();
	}
} NATIVE_ENTRY(call,"CALL",cell);

void inner_interpreter(void){
	size_t *old_inst_ptr = inst_ptr;
	inst_ptr = (size_t *)pop_data();
	size_t *inst_ptr_copy = inst_ptr;
	inst_ptr_copy++;
	do{
#ifdef DEBUG
		printf("calling with inst_ptr: %p\n", inst_ptr);
		show_proc_native();
#endif
		push_data(*(inst_ptr++));
		call_native();
	}
	while(inst_ptr != inst_ptr_copy);
	inst_ptr = old_inst_ptr;
}

NATIVE_CODE(number){
	char *empty = "";
	char **test = &empty;
	char *word = (char *)pop_data();
	size_t num = (size_t)strtol(word, test, 0);
	if('\0' == **test && *test > word)push_data(num);
	else {
		printf("%s is not a valid number\n", word);
		push_data((size_t)word);
		//recover_interpreter();
	}
} NATIVE_ENTRY(number,"#",call);

struct entry *head;//FORWARD DECLARATION

NATIVE_CODE(head){
	push_data((size_t)&head);
} NATIVE_ENTRY(head,"HEAD",number);

NATIVE_CODE(exec){//TODO use this to run threaded code from native code?
	char *word = (char *)pop_data();
	struct entry *current = head;
	while(current){
		if(!strcmp(word, current->name)){
			push_data((size_t)&current);
//			show_data_native();
//			show_proc_native();
//			here_native();
//			dot_native();
			inner_interpreter();
//			show_data_native();
//			show_proc_native();
//			here_native();
//			dot_native();
//			puts("jjjjjjjjjjjjj");
			return;
		}
		current = current->next;
	}
	push_data((size_t)word);
	number_native();
} NATIVE_ENTRY(exec,"EXEC",head);

static char word[0x100];

NATIVE_CODE(bsw){//bsw: blank-separated word
	int in;
	int word_index = 0;
	static char word[0x100];
	while(cin_native(),(in = (int)pop_data()) > ' '){
		word[word_index] = (char)in;
		word_index++;
		if(word_index >= 0x100)error_exit("word length >=256");
	}
	word[word_index] = '\0';
	push_data((size_t)word);
} NATIVE_ENTRY(bsw, "BSW", exec);

NATIVE_CODE(nprint){
	puts((char *)pop_data());
} NATIVE_ENTRY(nprint, "n.", bsw);

NATIVE_CODE(quote){
	bsw_native();
	char * word = (char *) pop_data();
	struct entry *current = head;
	while(current){
		if(!strcmp(word, current->name)){
			push_data((size_t)current);
			return;
		}
		current = current->next;
	}
	puts("********word not found!");
	return;
} NATIVE_ENTRY(quote, "'", nprint);

NATIVE_CODE(xaddr){
	struct entry *temp = (struct entry*)pop_data();
	if(0 == temp->code_size)error_exit("XADDR invalid for natives");
	push_data((size_t)temp->code.threaded);
} NATIVE_ENTRY(xaddr, "XADDR", quote);

NATIVE_CODE(seext){
	struct entry *xt = (struct entry*)pop_data();
	if(!xt->code_size){
		puts("Native code.");
		return;
	}
	size_t *code = xt->code.threaded;
	size_t *code_stop = code + xt->code_size;
	if(xt->code_size < 0)code_stop = code_ptr;//for unfinished words
	printf("%d\n",xt->code_size);
	for(;code < code_stop; ++code){
		struct entry *current = head;
		for(; current && current != (struct entry*)(*code); current = current->next);
		if(current){
			printf("%s ", current->name);
//			if(&literal_entry == current){
//				printf("%d ",*(++code));
//			}
		}
		else printf("%zd ",*code);
	}
	puts("");
} NATIVE_ENTRY(seext,"seext",xaddr);

NATIVE_CODE(words){
	struct entry *current = head;
	while(current){
		printf("%s ", current->name);
		current = current->next;
	}
	puts("");
} NATIVE_ENTRY(words,"WORDS",seext);

NATIVE_CODE(prompt){
	size_t *ptr = data_stack;
	if(data_ptr - ptr > 5){
		printf("...");
		ptr = data_ptr - 5;
	}
	while(ptr < data_ptr)printf("%zd ", *ptr++);
	printf(" >>> ");
} NATIVE_ENTRY(prompt,"PROMPT",words);

NATIVE_CODE(outer_interpreter){
	if(stdin == global_in)prompt_native();//can pass the prompt function on the stack, calling using CALL, to allow a non-interactive session that has an empty prompt function passed in
//	while(1){
//		bsw_native();
//		exec_native();
//		//some way to go back one character, test if it is \n and prompt? How to catch EOF?
//	}
//	return;
	int word_index = 0;
	//printf("%s %d\n", __FILE__, __LINE__);
	while(1){
		cin_native();
		int in = (int)pop_data();
		if(in > ' '){//note that EOF (-1) is less than ' '
			word[word_index] = (char)in;
			word_index++;
		}
		else{
			if(word_index){
				word[word_index] = '\0';
				word_index = 0;
				//puts(word);
				//printf("%s %d\n", __FILE__, __LINE__);
				push_data((size_t)word);
				exec_native();
			}
			if('\n' == in && stdin == global_in)prompt_native();
		}
		if(word_index >= 0x100)error_exit("word length >256");
		if(EOF == in)break;
	}
} NATIVE_ENTRY(outer_interpreter, "OUTER_INTERPRETER", prompt);

NATIVE_CODE(allot){
	here_native();
	add_native();
	code_ptr_native();
	set_value_native();
	if(code_ptr >= code_end){//CPTR CEND < ifd{ RET }if
		here_native();
		m_set_end_native();
	}
} NATIVE_ENTRY(allot,"ALLOT",outer_interpreter);

NATIVE_CODE(compile){// ( x -- )
	here_native();//store code_ptr value
	push_data(sizeof(code_ptr));//cell_native();
	allot_native();
	set_value_native();
} NATIVE_ENTRY(compile, ",", allot);

NATIVE_CODE(start_threaded_code){
	static const int delimiter = ']';
	int in;
	int word_index = 0;
	int got_delimiter = 0;
	//printf("%s %d\n", __FILE__, __LINE__);
start:
	while(cin_native(),(in = (int)pop_data()) != EOF){
		if(in > ' '){
			got_delimiter = 0;
			if(delimiter == in && !word_index)got_delimiter = 1;
			word[word_index] = (char)in;
			word_index++;
		}
		else if(word_index){
			if(got_delimiter)return;
			word[word_index] = '\0';
			word_index = 0;
			break;
		}
		if(word_index >= 0x100)error_exit("word length >256");
	}
	if(in == EOF)error_exit("cannot compile, EOF");
#ifdef DEBUG
	printf("word to compile: %s\n", word);
#endif
	struct entry *current = head;
	//printf("%s %d\n", __FILE__, __LINE__);
	while(current){
		if(!strcmp(word, current->name)){
			push_data((size_t)current);
			compile_native();
			break;
		}
		current = current->next;
	}
	if(NULL != current)goto start;
	//printf("%s %d\n", __FILE__, __LINE__);
	push_data((size_t)word);
	number_native();
	push_data((size_t)&literal_entry);
	compile_native();
	compile_native();
	goto start;
} NATIVE_ENTRY(start_threaded_code, "[", compile);

NATIVE_CODE(colon){
	if(temp_entry_ptr > temp_entry_store + TEMP_ENTRY_MAX)error_exit("too many unfinalized labels, limit TEMP_ENTRY_MAX!!!");
	int in;
	int word_index = 0;
	while(cin_native(),(in = (int)pop_data()) != EOF){
		if(in > ' '){
			word[word_index] = (char)in;
			word_index++;
		}
		else if(word_index){
			word[word_index] = '\0';
			strcpy(temp_entry_name_ptr, word);
			temp_entry_ptr->name = temp_entry_name_ptr;
			temp_entry_ptr->code.threaded = code_ptr;
			temp_entry_ptr->can_delete = -1;
			temp_entry_ptr->code_size = -1;//mark unfinished
			temp_entry_ptr->next = head;
			head = temp_entry_ptr;
			temp_entry_ptr++;
			temp_entry_name_ptr += TEMP_ENTRY_NAME_LENGTH_MAX;
			return;
		}
		if(word_index >= 0x100)error_exit("word length >256");
	}
} NATIVE_ENTRY(colon, ":", start_threaded_code);

NATIVE_CODE(semicolon){
	//puts("jjjjjjjjjjjjjjjjjjjjjjj");
	//printf("%s\n", temp_entry_store->name);
	//*code_ptr = 0xFECC;
	//printf("%x\n", *code_ptr);
//	//printf("%x\n", *code_ptr + 8);
	//printf("%s %d\n", __FILE__, __LINE__);
	push_data((size_t)&ret_entry);//Always insert a RET, just in case one is missing.
	compile_native();
	//printf("%s %d\n", __FILE__, __LINE__);
	size_t* after_code = code_ptr;
	struct entry *internal_ptr = temp_entry_store;
	for(; internal_ptr < temp_entry_ptr; ++internal_ptr){
		size_t *old_code_ptr = code_ptr;
		push_data((size_t)strlen(internal_ptr->name) + 1);//0term
		allot_native();
		strcpy((char*)old_code_ptr,internal_ptr->name);
		internal_ptr->name = (char*)old_code_ptr;
		internal_ptr->code_size = after_code - internal_ptr->code.threaded;
//		printf("%s %d %d %d\n", internal_ptr->name, internal_ptr->code.threaded, after_code, internal_ptr->code_size);
		if(internal_ptr > temp_entry_store)internal_ptr->next = head;
		head = (struct entry*)code_ptr;//just after string
		push_data(sizeof(struct entry));
		allot_native();
		memcpy(head, internal_ptr, sizeof(struct entry));
	}
	temp_entry_ptr = temp_entry_store;
	temp_entry_name_ptr = temp_entry_name_store;
} NATIVE_ENTRY(semicolon, ";", colon);

NATIVE_CODE(skip_bl){
	const char delimiter = (char)pop_data();
	int in;
	int word_index = 0;
	int got_delimiter = 0;
	while(cin_native(),(in = (int)pop_data()) != EOF){
		if(in > ' '){
			got_delimiter = 0;
			if(delimiter == in && !word_index)got_delimiter = 1;
			word_index++;
		}
		else if(word_index){
			if(got_delimiter)return;
			word_index = 0;
		}
	}
} NATIVE_ENTRY(skip_bl, "SKIPBL", semicolon);

NATIVE_CODE(char){
	bsw_native();
	push_data((size_t)(((char *)pop_data())[0]));
} NATIVE_ENTRY(char, "char", skip_bl);

NATIVE_CODE(line_comment){
	int in;
	while(cin_native(),(in = (int)pop_data()) != EOF){
		if('\n' == in)return;
	}
} NATIVE_ENTRY(line_comment, "\\", char);

NATIVE_CODE(interactive_if){
//	static const char * end_word = "}";
//	if(read_data())goto execute;
//	while(bsw_native(), strcmp(end_word, (char *)pop_data())){
//	}
//	return;
	static const int delimiter = '}';
	int in;
	int word_index = 0;
	int got_delimiter = 0;
	if(read_data())goto execute;
	while(cin_native(),(in = (int)pop_data()) != EOF){
		if(in > ' '){
			got_delimiter = 0;
			if(delimiter == in && !word_index)got_delimiter = 1;
			word_index++;
		}
		else if(word_index){
			if(got_delimiter)return;
			word_index = 0;
		}
	}
	return;
execute:
	while(cin_native(),(in = (int)pop_data()) != EOF){
		if(in > ' '){
			got_delimiter = 0;
			if(delimiter == in && !word_index)got_delimiter = 1;
			word[word_index] = (char)in;
			word_index++;
		}
		else if(word_index){
			if(got_delimiter)return;
			word[word_index] = '\0';
			word_index = 0;
			push_data((size_t)word);
			exec_native();
		}
		if(word_index >= 0x100)error_exit("word length >256");
	}
} NATIVE_ENTRY(interactive_if, "iif{", line_comment);

struct entry *head = &interactive_if_entry;

int main(int argc, char** argv){
	page_size = (size_t)sysconf(_SC_PAGESIZE);
	size_t phys_pages = (size_t)sysconf(_SC_PHYS_PAGES);
	size_t pages_to_use = phys_pages >> 4;//Need free VM space; may avoid overflow
	data_store_size = pages_to_use * page_size;
	if(pages_to_use != data_store_size/page_size){//overflow may indicate PAE
		puts("physical memory larger than maximum size_t value!");
		data_store_size = ((size_t)1) << (__WORDSIZE - 4);
	}
	code_spc = (size_t*)mmap(NULL, data_store_size,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(MAP_FAILED == code_spc)error_exit("mmap failed for code_spc");
	code_end = (size_t*)((size_t)code_spc + page_size);//beware pointer size
	code_ptr = code_spc;

	global_in = fopen("core.hfs", "r");
	outer_interpreter_native();
	fclose(global_in); global_in = NULL;
//	global_in = fopen("hello.hfs", "r");
//	outer_interpreter_native();
//	fclose(global_in); global_in = NULL;
	if(1 == argc){
		global_in = stdin;
		outer_interpreter_native();
		return 0;
	}
	int ii = 1;
	for(; ii < argc; ++ii){
		if(strcmp(argv[ii], "-")){
			global_in = fopen(argv[ii], "r");
			if(!global_in){
				printf("file %s not found\n", argv[ii]);
				return 1;
			}
			outer_interpreter_native();
			fclose(global_in); global_in = NULL;
		}
		else{
			global_in = stdin;
			outer_interpreter_native();
		}
	}
	return 0;
}
