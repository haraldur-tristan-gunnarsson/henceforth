//A forth-like language lacking immediate words.
//Metacomment: ( a1 a2 - "word" - n ) means: takes address 2 and THEN address 1 from the data stack, takes a "word" from the input stream and then places an integer on the data stack. 'r:' indicates the return stack, c indicates a character or byte, b usually indicates a boolean and so on.

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
#ifdef DEBUG
	size_t *crasher = NULL;
	crasher = (size_t*)*crasher; //In DEBUG mode, errors should cause a crash, to aid use of debuggers like GDB.
#endif
	exit(-1);
}

struct entry{//This defines the format of each entry in this forth-like language's dictionary (a linked list), i.e. the language's 'words'.
	const struct entry *next;
	char *name;//Given a whitespace-delimited word, the outer interpreter checks this
	union{//Address to execute (for native words) or interpret with inner interpreter
		size_t *threaded;
		void (*native)(void);
	} code;
	int can_delete;//Words defined using the language's tools can be deleted; those dfined in C code (and so in the machine code executable file) should permanent (Would the effects of attempting to delete them be platform-dependent?).
	int code_size;//if zero, then native. negative invalid
};
//Convenience macroes for defining permanent/primitive 'words':{
#define NATIVE_CODE(CNAME) void CNAME##_native(void)//Before definition.
#define NATIVE_ENTRY(CNAME,NAME,NEXT) \
const struct entry CNAME##_entry ={ \
	.next = &NEXT##_entry, \
	.name = NAME, \
	.code.native = &CNAME##_native, \
	.code_size = 0, \
	.can_delete = 0, \
};//After definition.
#define THREADED_CODE(CNAME) size_t CNAME##_threaded[]//Before definition.
#define THREADED_ENTRY(CNAME,NAME,NEXT) \
const struct entry CNAME##_entry ={ \
	.next = &NEXT##_entry, \
	.name = NAME, \
	.code.threaded = CNAME##_threaded, \
	.code_size = (sizeof CNAME##_threaded)/(sizeof(size_t*)), \
	.can_delete = 0, \
};//After definition.
//}

void null_entry_code(void){
	error_exit("******ERROR: executed null entry \"\" at end of dictionary");
}
const struct entry null_entry ={
	.next = NULL,//Cannot use macro for this.
	.name = "",//Should not happen, but this entry should be invoked if the outer interpreter wants to run a word named "". May happen if whitespace-handling is buggy.
	.code.native = &null_entry_code,
	.code_size = 0,
	.can_delete = 0,
};

size_t *code_spc = NULL;//Start of main area for threaded code and data. 'Code space'.
size_t *code_ptr = NULL;//Pointer to current end of code space (as seen by the user).
size_t *code_end = NULL;//Pointer to current end of memory available to code_ptr.
size_t code_spc_max = 0;//Maximum size of region pointed (in)to by code_spc and code_ptr
size_t page_size = 0;
#define TEMP_ENTRY_MAX (0x20)
const struct entry temp_entry_store[TEMP_ENTRY_MAX];//Separate, limited dictionary used by colon (:) and semicolon (;) in order to (perhaps foolishly) provide internal labels and unstructured code facility within 'words'. Unfortunately, if makes recursion very awkward.
struct entry *temp_entry_ptr = (struct entry*)temp_entry_store;
struct entry *temp_entry_head = NULL;
#define TEMP_ENTRY_NAME_LENGTH_MAX 0x100
const char temp_entry_name_store[TEMP_ENTRY_MAX * TEMP_ENTRY_NAME_LENGTH_MAX];
char *temp_entry_name_ptr = (char*)temp_entry_name_store;
#define STACK_SIZE (0x10000)
const size_t data_stack[STACK_SIZE];
size_t *data_ptr = (size_t*)data_stack;
const size_t proc_stack[STACK_SIZE];
size_t *proc_ptr = (size_t*)proc_stack;
size_t *inst_ptr = NULL;//Instruction pointer. Be very careful with code that touches it.
size_t *c_stack_start = NULL;//To enable TRACE, need the memory range for the C stack.

NATIVE_CODE(exit){
	exit(0);
} NATIVE_ENTRY(exit,"exit",null);

NATIVE_CODE(reset_stack){
	data_ptr = (size_t*)data_stack;
} NATIVE_ENTRY(reset_stack,"reset_stack",exit);

NATIVE_CODE(align_code){//can be used to make dumps easier to read
	size_t aligner = sizeof(size_t) - 1 ;
	size_t temp = (size_t)(void*)code_ptr;//BEWARE pointer size
	temp +=  aligner;
	temp &= ~aligner;
	code_ptr = (size_t*)(void*)temp;
} NATIVE_ENTRY(align_code,"ALIGN_CODE",reset_stack);

NATIVE_CODE(show_data){//Display contents of data stack.
	size_t *ii;
	printf("data: ");
	for(ii = (size_t*)data_stack; ii < data_ptr; ++ii)printf("%zd ", *ii);
	puts("");
} NATIVE_ENTRY(show_data,".s",align_code);

NATIVE_CODE(show_proc){//Display contents of return stack.
	size_t *ii;
	printf("proc: ");
	for(ii = (size_t*)proc_stack; ii < proc_ptr; ++ii)printf("%zd ", *ii);
	puts("");
} NATIVE_ENTRY(show_proc,".r",show_data);

void push_data(size_t value){//Push value onto data stack.
#ifdef DEBUG
	show_data_native();//When debugging, show contents of data stack before...
#endif
	*data_ptr = value;
	data_ptr++;
	if(data_ptr>data_stack+STACK_SIZE)error_exit("data stack overflow");
#ifdef DEBUG
	show_data_native();//...and after changing it.
#endif
}
size_t pop_data(void){//Pop value from data stack.
#ifdef DEBUG
	show_data_native();//When debugging, show contents of data stack before...
#endif
	data_ptr--;
	if(data_ptr<data_stack)error_exit("data stack underflow");
#ifdef DEBUG
	show_data_native();//...and after changing it.
#endif
	return *data_ptr;
}
size_t read_data(void){return *(data_ptr - 1);}

void push_proc(size_t value){//Push value onto return stack.
#ifdef DEBUG
	show_proc_native();//When debugging, show contents of return stack before...
#endif
	*proc_ptr = value;
	proc_ptr++;
	if(proc_ptr>proc_stack+STACK_SIZE)error_exit("proc stack overflow");
#ifdef DEBUG
	show_proc_native();//...and after changing it.
#endif
}
size_t pop_proc(void){//Pop value from return stack.
#ifdef DEBUG
	show_proc_native();//When debugging, show contents of return stack before...
#endif
	proc_ptr--;
	if(proc_ptr<proc_stack)error_exit("proc stack underflow");
#ifdef DEBUG
	show_proc_native();//...and after changing it.
#endif
	return *proc_ptr;
}
size_t read_proc(void){return *(proc_ptr - 1);}


#define VARIABLE(CNAME,NAME,NEXT) NATIVE_CODE(CNAME){ \
	push_data((size_t)(&(CNAME))); \
} NATIVE_ENTRY(CNAME,NAME,NEXT);//( -- system_variable ) Read 'var @'. Write 'val var !'.

#define BINARY_OP(CNAME,OP,NAME,NEXT) NATIVE_CODE(CNAME){ \
	size_t val2 = pop_data(); \
	size_t val1 = pop_data(); \
	push_data(val1 OP val2); \
} NATIVE_ENTRY(CNAME,NAME,NEXT);//( n1 n2 -- n1OPn2 ) For C-derived binary op primitives.

NATIVE_CODE(not){//( n -- notn )
	push_data(pop_data() ? 0 : ~0);// ~0 (-1) is TRUE, 0 is FALSE
} NATIVE_ENTRY(not, "NOT", show_proc);

#define BOOLEAN_OP(CNAME,OP,NAME,NEXT) NATIVE_CODE(CNAME){ \
	size_t val2 = pop_data(); \
	size_t val1 = pop_data(); \
	push_data(val1 OP val2); \
	not_native(); \
	not_native(); \
} NATIVE_ENTRY(CNAME,NAME,NEXT);//( n1 n2 -- n1OPn2 ) For C-derived boolean operations.
//Note that, in C, TRUE is 1, while I want TRUE to be ~0 (-1); hence NOT NOT at the end.

BOOLEAN_OP(eq,  =,  "=",  not);// : = [ - NOT ] ;
BOOLEAN_OP(neq, !=, "<>", eq);// : <> [ - NOT NOT ] ;
BOOLEAN_OP(gt,  >,  ">",  neq);//Unsigned...
BOOLEAN_OP(lt,  <,  "<",  gt);//...
BOOLEAN_OP(ge,  >=, ">=", lt);//...
BOOLEAN_OP(le,  <=, "<=", ge);//...

//Binary operators for language:
BINARY_OP(mul,  *,  "*",    le);
BINARY_OP(div,  /,  "/",    mul);
BINARY_OP(mod,  %,  "%",    div);
BINARY_OP(sub,  -,  "-",    mod);
BINARY_OP(add,  +,  "+",    sub);
BINARY_OP(shl,  <<, "<<",   add);//Logical, add arithmetic shift later...
BINARY_OP(shr,  >>, ">>",   shl);//...
BINARY_OP(bor,  |,  "BOR",  shr);
BINARY_OP(bxor, ^,  "BXOR", bor);
BINARY_OP(band, &,  "BAND", bxor);

NATIVE_CODE(bnot){//( n -- ~n )
	push_data(~pop_data());
} NATIVE_ENTRY(bnot,"BNOT",band);

struct entry *head;//FORWARD DECLARATION
//System variables, to be used (with caution) inside forth-like code:
VARIABLE(head,     "HEAD", bnot);
VARIABLE(code_ptr, "CPTR", head);//In addition to HERE, for modification.
VARIABLE(code_end, "CEND", code_ptr);
VARIABLE(data_ptr, "DPTR", code_end);
VARIABLE(proc_ptr, "PPTR", data_ptr);
VARIABLE(global_in, "GLOBAL_INPUT", proc_ptr);

NATIVE_CODE(literal){//Get next code value and skip it. Needed for numerical constants.
	push_data(*inst_ptr++);//( -- n )
} NATIVE_ENTRY(literal,"LITERAL",global_in);

NATIVE_CODE(ret){//Return from called word to calling (virtual) word
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
} NATIVE_ENTRY(ret,"RET",literal);

THREADED_CODE(cell) = {//( -- n ) gives size of both items on the stacks and in 'code'
	(size_t)&literal_entry,
	sizeof data_ptr,
	(size_t)&ret_entry,
}; THREADED_ENTRY(cell,"CELL",ret);

NATIVE_CODE(stdin){//( -- page_size )
	push_data((size_t)stdin);
} NATIVE_ENTRY(stdin,"STDIN",cell);//CONSTANT macro, like HERE, CELL?

NATIVE_CODE(pagesize){//( -- page_size )
	push_data(page_size);
} NATIVE_ENTRY(pagesize,"PAGESIZE",stdin);//CONSTANT macro, like HERE, CELL?


NATIVE_CODE(cin){//( - c - ) Get next character from input stream.
	push_data((size_t)getc(global_in));
} NATIVE_ENTRY(cin,"CIN",pagesize);

NATIVE_CODE(at){//( addr -- n ) A size_t pointer dereference for top value on data stack.
	push_data(*((size_t *)pop_data()));
} NATIVE_ENTRY(at,"@",cin);

NATIVE_CODE(set_value){//( n addr -- ) Set size_t pointed to by the top data stack item.
	size_t *ref = (size_t *)pop_data();
	size_t val = pop_data();
	*ref = val;
} NATIVE_ENTRY(set_value,"!",at);

NATIVE_CODE(char_at){//( addr -- n ) Like @/at, but for a single character/byte.
	push_data((size_t)(*((char *)pop_data())));
} NATIVE_ENTRY(char_at,"c@",set_value);

NATIVE_CODE(char_set_value){//( n addr -- ) Like !/set_value, but for a single char/byte.
	char *ref = (char *)pop_data();
	char val = (char)pop_data();
	*ref = val;
} NATIVE_ENTRY(char_set_value,"c!",char_at);

NATIVE_CODE(m_set_end){//( addr -- ) Handles virtual memory for the 'code space'.
	size_t addr = pop_data();	//10110111	initial address (example)
	size_t temp = page_size - 1;	//00001111	for tiny (16 byte) page size
	addr +=  temp;			//11000110
	addr &= ~temp;			//11000000	ALIGNED!
	if(addr >= (size_t)code_spc + code_spc_max)error_exit("data overflow: mremap");
	code_end = (size_t*)addr;
	size_t trimmed_size = addr - (size_t)code_spc;//beware pointer size
	//The Linux kernel's virtual memory manager is "lazy" -- it only allocates
	//physical memory to a process once it is read from or written to.
	//Here, memory beyond the new address is freed from this process and then
	//the freed virtual memory address range is reclaimed.
	//This prevents the kernel from placing anything else within the address range,
	//as that would prevent the 'code space' from expanding into the address range.
	void *res = MAP_FAILED;
	res = mremap(code_spc, code_spc_max, trimmed_size, 0);
	if(MAP_FAILED == res)error_exit("mremap failed for m_set_end (first time)");
	res = mremap(code_spc, trimmed_size, code_spc_max, 0);
	if(MAP_FAILED == res)error_exit("mremap failed for m_set_end (second time)");
} NATIVE_ENTRY(m_set_end,"M_SET_END",char_set_value);

NATIVE_CODE(drop){//( n -- )
	(void)pop_data();
} NATIVE_ENTRY(drop,"DROP",m_set_end);

NATIVE_CODE(rdrop){//( r: addr -- )
	(void)pop_proc();
} NATIVE_ENTRY(rdrop,"RDROP",drop);

NATIVE_CODE(swap){//( n1 n2 -- n2 n1 )
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val2);
	push_data(val1);
} NATIVE_ENTRY(swap,"SWAP",rdrop);

NATIVE_CODE(s_to_r){//( n -- ) ( r: -- n )
	push_proc(pop_data());
} NATIVE_ENTRY(s_to_r,">r",swap);

NATIVE_CODE(r_to_s){//( r: addr -- ) ( -- addr )
	push_data(pop_proc());
} NATIVE_ENTRY(r_to_s,"r>",s_to_r);

NATIVE_CODE(s_to_r_copy){//( n -- n ) ( r: -- n )
	push_proc(read_data());
} NATIVE_ENTRY(s_to_r_copy,"@r",r_to_s);

NATIVE_CODE(r_to_s_copy){//( r: addr -- addr ) ( -- addr )
	push_data(read_proc());
} NATIVE_ENTRY(r_to_s_copy,"r@",s_to_r_copy);

NATIVE_CODE(here){// ( -- addr ) CPTR @
	code_ptr_native();
	at_native();
} NATIVE_ENTRY(here,"HERE",r_to_s_copy);

NATIVE_CODE(decprint){//( n -- )
	printf("%zd",pop_data());
} NATIVE_ENTRY(decprint,"(.)",here);

NATIVE_CODE(hexprint){//( n -- )
	printf("%zx",pop_data());
} NATIVE_ENTRY(hexprint,"(x.)",decprint);

NATIVE_CODE(emit){//( c -- )
	putchar((char)pop_data());
} NATIVE_ENTRY(emit, "emit", hexprint);

NATIVE_CODE(trace){//Display contents of return stack as 'word' names for debugging.
	size_t **ii;//Should be pointer to the same type as inst_ptr...
	printf("trace: ");//...as the proc_stack is often a string of inst_ptr values.
	size_t *c_stack_end = (size_t*)&ii;//Needed for first word TRACED.
	for(ii = (size_t**)proc_stack; ii < (size_t**)proc_ptr; ++ii){
		size_t *past_inst_ptr = *ii - 1;//Should be the same type as inst_ptr.
		//It would be nice if there were a syscall to tell whether a certain memory address was mapped for the calling process. Is there one? TODO.
		//Need to test the C stack as the inner interpreter's first XT is on it.
		//If neither in used part of code_spc nor in C stack (which grows down):
		if((past_inst_ptr < code_spc	|| past_inst_ptr >= code_ptr)
		&&( past_inst_ptr < c_stack_end	|| past_inst_ptr >= c_stack_start)){
			printf("%zd ", (size_t)*ii);//Probably a value put on stack.
			continue;//Otherwise, '*' would probably SEGFAULT.
		}//Normally, the 'code space' is the right area for the inst_ptr.
		struct entry *test = (struct entry*)(*past_inst_ptr);
		struct entry *current = head;
		for(;current && current != test; current = (struct entry*)current->next);
		if(current && &null_entry != current)printf("%s ", current->name);
		else printf("%zd ", (size_t)*ii);
	}
	puts("");
} NATIVE_ENTRY(trace,"TRACE",emit);

NATIVE_CODE(call){// ( xt -- ) given execution token (xt, entry) execute its code
	struct entry *xt = (struct entry *)pop_data();
#ifdef DEBUG
	printf("inst_ptr: %p after %s (%p->%p)\n", inst_ptr, xt->name, xt, (void*)xt->code.threaded);
	if(!xt->code_size)puts("native");
#endif
	if(xt->code_size){//For threaded code:
		push_proc((size_t)inst_ptr);//Push current inst_ptr to return stack
		inst_ptr = xt->code.threaded;//inst_ptr <-- beginning of new word's code
	}
	else{//For native code:
		xt->code.native();
	}
} NATIVE_ENTRY(call,"CALL",trace);

void inner_interpreter(void){//( &xt -- ) The CORE of the forth-like interpreter.
	//This function executes strings of execution tokens (xt) i.e. 'threaded code'.
	//Each xt is a pointer to a dictionary (linked-list) entry for code.
	//Each entry can point to threaded code or to native code.
	//Native code is executed immediately by CALL.
	//For threaded code, xts call other xts until native code is reached.
	//Calling threaded xts fills the return stack, while calls to RET empty it.
	//When working properly, this function terminates when the return stack...
	//...reaches a state identical to that after the initial xt pointer is popped.
	//I.e. ( &xt -- ).
	size_t *old_inst_ptr = inst_ptr;
	inst_ptr = (size_t *)pop_data();//Effectively 'struct entry **xt'.
	size_t *next_xt = inst_ptr + 1;//Probably invalid. Do not execute.
	do{//Until next_xt (delayed by threaded CALLs until equal number of RETs):
#ifdef DEBUG
		printf("calling with inst_ptr: %p\n", inst_ptr);
		show_proc_native();
		trace_native();
#endif
		push_data(*(inst_ptr++));//Pushes 'struct entry *xt' then points to next.
		call_native();
	}
	while(inst_ptr != next_xt);//Do not execute (probably invalid) next_xt...
	inst_ptr = old_inst_ptr;//...rather, return to initial state.
}

NATIVE_CODE(number){// ( char* -- n ) Converts a null-terminated string to an integer.
	char *test = "";
	char *word = (char *)pop_data();
	size_t num = (size_t)strtol(word, &test, 0);
	if('\0' == *test && '\0' != *word)push_data(num);
	else {
		printf("%s is not a valid number\n", word);
		push_data(0);//Is there a better way? At least if this is used in calculations then results of '0' may arouse suspicions.
	}
} NATIVE_ENTRY(number,"#",call);

NATIVE_CODE(bsw){//( -- "word" ) BSW: blank-separated word, null-terminated.
	int word_index = 0;
	static char word[0x100];
	while(1){
		cin_native();
		int in = (int)pop_data();
		if(in > ' '){//note that EOF (-1) is less than ' '
			word[word_index] = (char)in;//Add non-whitespace character.
			word_index++;
		}
		else if(word_index)break;//Allows/ignores preceding whitespace.
		if(word_index > 0xfe)error_exit("word length >254");
		if(in == EOF)error_exit("cannot continue, EOF");
	}
	word[word_index] = '\0';
	push_data((size_t)word);
} NATIVE_ENTRY(bsw, "BSW", number);

NATIVE_CODE(nprint){//( "string" -- ) print null-terminated string.
	fputs((char *)pop_data(), stdout);
} NATIVE_ENTRY(nprint, "(n.)", bsw);

NATIVE_CODE(wordsearch){//( "word" -- xt ) Find (latest, most recent) xt for given word.
	char *word = (char *) pop_data();
	struct entry *current = head;
	while(current){
		if(!strcmp(word, current->name)){//If identical, xt found.
			break;
		}
		current = (struct entry*)current->next;
	}
	push_data((size_t)current);//WILL return null_entry if empty string "" given.
} NATIVE_ENTRY(wordsearch, "WORDSEARCH", nprint);

NATIVE_CODE(exec){//( "word" -- | n ) Run given word (i.e. null-terminated string).
	//TODO use this to run threaded code from native code?
	push_data((size_t)read_data());			//~ DUP ( "word" "word )
	wordsearch_native();				//~ WORDSEARCH ( "word" xt )
	if(read_data()){//If the word exists, execute:	//if{
		swap_native();				//~ SWAP ( xt "word" )
		drop_native();				//~ DROP ( xt ) \ NIP?
		size_t xt = pop_data();			//How does one create a...
		push_data((size_t)&xt);			//...reference in Forth?
		inner_interpreter();//For some reason, crashes if DROP is AFTER this.
	}
	else{//Else attempt to convert to an integer:	//}elsed{ \ includes DROP
		drop_native();
		number_native();			//~ #
	}
} NATIVE_ENTRY(exec, "EXEC", wordsearch);

NATIVE_CODE(quote){//( - "word" - xt ) Get subsequent word's xt.
	bsw_native();		//~ BSW
	wordsearch_native();	//~ WORDSEARCH
	size_t xt = pop_data();	//if{ ~ RET }if ~ DROP ( print out something ) ~ RET
	if(xt)push_data(xt);
	else puts("********word not found!");//If wanting to allow nulls: BSW WORDSEARCH
} NATIVE_ENTRY(quote, "'", exec);

NATIVE_CODE(xaddr){//( xt -- xaddr ) Get execution address of threaded code.
	struct entry *temp = (struct entry*)pop_data();
	if(0 == temp->code_size)error_exit("XADDR invalid for natives");
	push_data((size_t)temp->code.threaded);
} NATIVE_ENTRY(xaddr, "XADDR", quote);

NATIVE_CODE(name_out){//( xt -- ) Print out the xt's name or, failing that, its value
	struct entry *test = (struct entry*)pop_data();
	struct entry *current = head;
	for(; current && current != test; current = (struct entry*)current->next);//Try to find entry.
	if(current && &null_entry != current)printf("%s", current->name);
	else printf("%zd",(size_t)test);
} NATIVE_ENTRY(name_out, "NAME_OUT", xaddr);

NATIVE_CODE(seext){//( xt -- ) Print out the xt's code length and token string (code).
	struct entry *xt = (struct entry*)pop_data();
	if(!xt->code_size){//Native code has no token string, and has code_size of 0.
		puts("Native code.");
		return;
	}
	size_t *code = xt->code.threaded;
	size_t *code_stop = code + xt->code_size;
	if(xt->code_size < 0)code_stop = code_ptr;//For unfinished words (no semicolon).
	printf("%d\n",xt->code_size);
	for(;code < code_stop; ++code){
		push_data((size_t)*code);
		name_out_native();
		putchar(' ');
	}
	putchar('\n');
} NATIVE_ENTRY(seext,"seext",name_out);

NATIVE_CODE(xname){//( xt -- "name" )
	struct entry *xt = (struct entry*)pop_data();
	push_data((size_t)xt->name);
} NATIVE_ENTRY(xname,"XNAME",seext);

NATIVE_CODE(words){//Print out the names of all words. Many definitions --> words repeat.
	struct entry *current = head;
	while(current){
		printf("%s ", current->name);//Up the linked list!
		current = (struct entry*)current->next;
	}
	puts("");
} NATIVE_ENTRY(words,"WORDS",xname);

NATIVE_CODE(prompt){//Print out a nice (?) prompt for interactive use. Imperfect.
	size_t *ptr = (size_t*)data_stack;
	if(data_ptr - ptr > 5){
		printf("...");
		ptr = data_ptr - 5;
	}
	while(ptr < data_ptr)printf("%zd ", *ptr++);
	printf(" >>> ");
} NATIVE_ENTRY(prompt,"PROMPT",words);

NATIVE_CODE(outer_interpreter){//This is what the user interacts with.
	if(stdin == global_in)prompt_native();//can pass the prompt function on the stack, calling using CALL, to allow a non-interactive session that has an empty prompt function passed in
//	while(1){
//		bsw_native();
//		exec_native();
//		//some way to go back one character, test if it is \n and prompt? How to catch EOF?
//	}
//	return;
	char word[0x100];
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

NATIVE_CODE(allot){//( n -- ) Allocate given amount of bytes of memory to 'code space'.
	here_native();			//~ HERE
	add_native();			//~ +
	code_ptr_native();		//~ CPTR
	set_value_native();		//~ !
	if(code_ptr >= code_end){	//[ HERE CEND @ < ] ifd{ ~ RET }if
		here_native();		//~ HERE
		m_set_end_native();	//~ M_SET_END
	}				//~ RET
} NATIVE_ENTRY(allot,"ALLOT",outer_interpreter);

NATIVE_CODE(compile){//( xt -- )
	here_native();//store code_ptr value on data stack	//~ HERE
	push_data(sizeof(code_ptr));//cell_native();		//~ CELL
	allot_native();//modifies code_ptr			//~ ALLOT
	set_value_native();//store xt in stored addr from HERE	//~ ! ( xt addr -- )
} NATIVE_ENTRY(compile, ",", allot);

NATIVE_CODE(cstrcmp){//( s1 s2 -- n ) Forthy interface for C's strcmp().
	char *s2 = (char *)pop_data();
	char *s1 = (char *)pop_data();
	push_data((size_t)strcmp(s1, s2));
} NATIVE_ENTRY(cstrcmp, "CSTRCMP", compile);

NATIVE_CODE(start_threaded_code){//Until delimiter ("]") is reached, record into threaded code the execution tokens of given words or (for numbers) LITERAL plus the value.
							//,nh" }" value delimiter : iifd{
	while(1){						//infloop{
		bsw_native();					//~ BSW
		push_data((size_t)read_data());			//~ DUP
		push_data((size_t)"]");				//~ delimiter
		cstrcmp_native();				//~ CSTRCMP
		if(!pop_data()){				//~ NOT ifd{
			(void)pop_data();			//~ DROP
			break;					//~ RET
		}						//}if
		push_data((size_t)read_data());			//~ DUP ( "word" "word )
		wordsearch_native();				//~ WORDSEARCH
		if(read_data()){//For words:			//if{
			compile_native();			//~ ,
			drop_native();				//~ DROP
		}
		else{//For numbers:				//}elsed{ \ includes DROP
			drop_native();				//~ [LITERAL] or...
			push_data((size_t)&literal_entry);	//...~~ LITERAL
			compile_native();//'LITERAL value': push 'value' --> data stack
			number_native();			//~ #
			compile_native();			//~ ,
		}						//}if
	}							//}infloop ;
} NATIVE_ENTRY(start_threaded_code, "[", cstrcmp);

NATIVE_CODE(colon){//( - "name" - ) Create a temporary entry. Made permanent by ;.
	if(temp_entry_ptr > temp_entry_store + TEMP_ENTRY_MAX)error_exit("too many unfinalized labels, limit TEMP_ENTRY_MAX!!!");
	int in;
	char word[0x100];
	int word_index = 0;
	while(cin_native(),(in = (int)pop_data()) != EOF){
		if(in > ' '){
			word[word_index] = (char)in;
			word_index++;
		}
		else if(word_index){
			word[word_index] = '\0';
			strcpy(temp_entry_name_ptr, word);//Send name to temporary store.
			temp_entry_ptr->name = temp_entry_name_ptr;
			temp_entry_ptr->code.threaded = code_ptr;//ALL from now to ;.
			temp_entry_ptr->can_delete = -1;//Yes.
			temp_entry_ptr->code_size = -1;//Mark unfinished.
			temp_entry_ptr->next = head;
			head = temp_entry_ptr;
			temp_entry_ptr++;
			temp_entry_name_ptr += TEMP_ENTRY_NAME_LENGTH_MAX;
			return;
		}
		if(word_index >= 0x100)error_exit("word length >256");
	}
} NATIVE_ENTRY(colon, ":", start_threaded_code);

NATIVE_CODE(semicolon){//Make all currently temporary dictionary entries permanent.
	//Typical layout: threaded code -- name string -- dictionary entry.
	push_data((size_t)&ret_entry);//Always insert a RET, just in case one is missing.
	compile_native();//~~ RET
	size_t* after_code = code_ptr;
	struct entry *internal_ptr = (struct entry*)temp_entry_store;
	for(; internal_ptr < temp_entry_ptr; ++internal_ptr){//For all temp entries (1?)
		char *name_ptr = (char*)code_ptr;//Place name before struct entry.

		push_data((size_t)strlen(internal_ptr->name) + 1);//strlen excludes 0term
		allot_native();//Changes code_ptr --> after_code.
		strcpy(name_ptr,internal_ptr->name);
		align_code_native();//Make dumps readable.

		internal_ptr->name = name_ptr;
		internal_ptr->code_size = after_code - internal_ptr->code.threaded;
		if(internal_ptr > temp_entry_store)internal_ptr->next = head;//Not first.
		head = (struct entry*)code_ptr;//Just after name.

		push_data(sizeof(struct entry));
		allot_native();//Changes code_ptr --> after_code.
		memcpy(head, internal_ptr, sizeof(struct entry));
		//align_code_native();//Not necessary at time of coding, assuming that an 'int' is either equal to or half the size of a pointer and/or structs are padded automatically to align to the largest primitive member type (i.e. 'size_t') -- otherwise, or if the 'entry' struct is changed, this may be necessary for pleasant and useful memory dumps.
	}
	temp_entry_ptr = (struct entry*)temp_entry_store;//Clear temporary entries...
	temp_entry_name_ptr = (char*)temp_entry_name_store;//...
} NATIVE_ENTRY(semicolon, ";", colon);

NATIVE_CODE(skip_bl){//( c -- ) Given delimiter, skip input to space-delimited delimiter.
//	//Version using a string:
//	while(1){				//infloop{
//		push_data((size_t)read_data());	//~ DUP
//		bsw_native();			//~ BSW
//		cstrcmp_native();		//~ CSTRCMP ( "delimiter" n )
//		if(!pop_data()){		//~ NOT ifd{
//			(void)pop_data();	//~ DROP
//			break;			//~ RET
//		}				//}if
//	}					//}infloop
	const char delimiter = (char)pop_data();
	int in;
	int word_index = 0;
	int got_delimiter = 0;
	while(cin_native(),(in = (int)pop_data()) != EOF){
		if(in > ' '){
			got_delimiter = 0;//No delimiter unless sole character in word.
			if(delimiter == in && !word_index)got_delimiter = 1;
			word_index++;
		}
		else if(word_index){
			if(got_delimiter)break;
			word_index = 0;
		}
	}
} NATIVE_ENTRY(skip_bl, "SKIPBL", semicolon);

NATIVE_CODE(line_comment){
	int in;
	while(cin_native(),(in = (int)pop_data()) != EOF){
		if('\n' == in)break;
	}
} NATIVE_ENTRY(line_comment, "\\", skip_bl);

struct entry *head = (struct entry*)&line_comment_entry;

int main(int argc, char** argv){
	//Setting up MMAP for main threaded-code memory area:
	page_size = (size_t)sysconf(_SC_PAGESIZE);
	size_t phys_pages = (size_t)sysconf(_SC_PHYS_PAGES);
	size_t pages_to_use = phys_pages >> 4;//Need free virtual memory space; need less than all of it (also avoids overflow if the maximum value of size_t is the same as phys_pages * page_size)
	code_spc_max = pages_to_use * page_size;
	if(pages_to_use != code_spc_max/page_size){//overflow --> PAE? 32-bit compat?
		puts("physical memory larger than maximum size_t value!");
		code_spc_max = ((size_t)1) << (__WORDSIZE - 4);
	}
	code_spc = (size_t*)mmap(NULL, code_spc_max,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(MAP_FAILED == code_spc)error_exit("mmap failed for code_spc");
	code_end = (size_t*)((size_t)code_spc + page_size);//Beware pointer size
	code_ptr = code_spc;
	c_stack_start = &phys_pages;

	global_in = fopen("core.hfs", "r");//Set up core functions (words) from source...
	outer_interpreter_native();//...
	fclose(global_in); global_in = NULL;//...

	if(1 == argc){//Interactive mode:
		global_in = stdin;
		outer_interpreter_native();
		return 0;
	}
	int ii = 1;
	for(; ii < argc; ++ii){
		if(strcmp(argv[ii], "-")){//Interpreting source files:
			global_in = fopen(argv[ii], "r");
			if(!global_in){
				printf("file %s not found\n", argv[ii]);
				return 1;
			}
			outer_interpreter_native();
			fclose(global_in); global_in = NULL;
		}
		else{//Interactive mode:
			global_in = stdin;
			outer_interpreter_native();
		}
	}
	return 0;
}
