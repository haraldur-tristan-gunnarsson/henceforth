#include <stdio.h>
#include <stdlib.h>//malloc etc.
#include <stddef.h>//size_t etc.
#include <string.h>//strcpy etc.
#include <unistd.h>//sbrk

//#define DEBUG

void error_exit(char *message){
	puts(message);
	exit(-1);
}

struct entry{
	char *name;
	union{
		size_t *threaded;
		void (*native)(void);
	} code;
	int can_delete;
	int code_size;//if zero, then native. negative invalid
	struct entry *next;
};
#define NATIVE_CODE(CNAME) void CNAME##_native(void)
#define NATIVE_ENTRY(CNAME,NAME,NEXT) \
struct entry CNAME##_entry ={ \
	.name = NAME, \
	.code.native = &CNAME##_native, \
	.code_size = 0, \
	.can_delete = 0, \
	.next = &NEXT##_entry, \
};
#define THREADED_CODE(CNAME) size_t CNAME##_threaded[]
#define THREADED_ENTRY(CNAME,NAME,NEXT) \
struct entry CNAME##_entry ={ \
	.name = NAME, \
	.code.threaded = CNAME##_threaded, \
	.code_size = (sizeof CNAME##_threaded)/(sizeof(size_t*)), \
	.can_delete = 0, \
	.next = &NEXT##_entry, \
};

void null_entry_code(void){
	error_exit("******ERROR: executed null entry \"\" at end of dictionary");
}
struct entry null_entry ={
	.name = "",
	.code.native = &null_entry_code,
	.code_size = 0,
	.can_delete = 0,
	.next = NULL,
};

#define STACK_SIZE (0x10000)
size_t code_space[STACK_SIZE];
size_t *code_ptr = code_space;
struct entry *temp_code = NULL;
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
	for(ii = data_stack; ii < data_ptr; ++ii)printf("%d ", *ii);
	puts("");
} NATIVE_ENTRY(show_data,".s",reset_stack);

NATIVE_CODE(show_proc){
	size_t *ii;
	printf("proc: ");
	for(ii = proc_stack; ii < proc_ptr; ++ii)printf("%d ", *ii);
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

NATIVE_CODE(ret){// r> IPTR !
#ifdef DEBUG
	show_data_native();
	show_proc_native();
	printf("%d %s\n", __LINE__, __FILE__);
	printf("inst_ptr: %u\n", inst_ptr);
#endif
	inst_ptr = (size_t*)pop_proc();
#ifdef DEBUG
	printf("inst_ptr: %u\n", inst_ptr);
#endif
} NATIVE_ENTRY(ret,"RET",show_proc);

NATIVE_CODE(literal){
	push_data(*inst_ptr++);
} NATIVE_ENTRY(literal,"LITERAL",ret);

NATIVE_CODE(sbrk){
	size_t old_end = (size_t)sbrk(pop_data());
	if(~old_end)push_data(old_end);//error if -1, i.e ~old_end is 0
	else error_exit("SBRK error!");
} NATIVE_ENTRY(sbrk,"SBRK",literal);

NATIVE_CODE(drop){
	(void)pop_data();
} NATIVE_ENTRY(drop,"DROP",sbrk);

NATIVE_CODE(rdrop){
	(void)pop_proc();
} NATIVE_ENTRY(rdrop,"RDROP",drop);

NATIVE_CODE(dup){// >r r@ r>
	push_data(read_data());
} NATIVE_ENTRY(dup,"DUP",rdrop);

NATIVE_CODE(swap){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val2);
	push_data(val1);
} NATIVE_ENTRY(swap,"SWAP",dup);

NATIVE_CODE(s_to_r){
	push_proc(pop_data());
} NATIVE_ENTRY(s_to_r,">r",swap);

NATIVE_CODE(r_to_s){
	push_data(pop_proc());
} NATIVE_ENTRY(r_to_s,"r>",s_to_r);

NATIVE_CODE(r_to_s_copy){
	push_data(read_proc());
} NATIVE_ENTRY(r_to_s_copy,"r@",r_to_s);

NATIVE_CODE(inst_ptr){
	push_data((size_t)(&inst_ptr));
} NATIVE_ENTRY(inst_ptr,"IPTR",r_to_s_copy);

NATIVE_CODE(code_ptr){
	push_data((size_t)(&code_ptr));
} NATIVE_ENTRY(code_ptr,"CPTR",inst_ptr);

NATIVE_CODE(code_space){
	push_data((size_t)(code_space));
} NATIVE_ENTRY(code_space,"CSPA",code_ptr);

NATIVE_CODE(at){
	push_data(*((size_t *)pop_data()));
} NATIVE_ENTRY(at,"@",code_space);

NATIVE_CODE(set_value){
	size_t *ref = (size_t *)pop_data();
	size_t val = pop_data();
	*ref = val;
} NATIVE_ENTRY(set_value,"!",at);

NATIVE_CODE(dot){
	printf("%d\n",pop_data());
} NATIVE_ENTRY(dot,".",set_value);

NATIVE_CODE(malloc){
	push_data((size_t)malloc((size_t)pop_data()));
} NATIVE_ENTRY(malloc,"MALLOC",dot);

NATIVE_CODE(free){
	free((void *)pop_data());
} NATIVE_ENTRY(free,"FREE",malloc);

NATIVE_CODE(mul){
	size_t val2 = pop_data();
	size_t val1 = pop_data();
	push_data(val1*val2);
} NATIVE_ENTRY(mul,"*",free);

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

THREADED_CODE(cell) = {
	(size_t)&literal_entry,
	sizeof data_ptr,
	(size_t)&ret_entry,
}; THREADED_ENTRY(cell,"CELL",add);

NATIVE_CODE(call){
	struct entry *xt = (struct entry *)pop_data();
#ifdef DEBUG
	printf("inst_ptr: %u after %s (%u->%u)\n", inst_ptr, xt->name, xt, xt->code);
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
		printf("calling with inst_ptr: %u\n", inst_ptr);
		show_proc_native();
#endif
		push_data(*(inst_ptr++));
		call_native();
	}
	while(inst_ptr != inst_ptr_copy);
	inst_ptr = old_inst_ptr;
}

struct entry *head;//FORWARD DECLARATION

NATIVE_CODE(exec){
	char *word = (char *)pop_data();
	struct entry *current = head;
	while(current){
		if(!strcmp(word, current->name)){
			push_data((size_t)&current);
			inner_interpreter();
			return;
		}
		current = current->next;
	}
	push_data(atoi(word));//XXX TODO temporary, errors give 0
//	puts("********word not found!");
} NATIVE_ENTRY(exec,"EXEC",call);

static char word[0x100];

NATIVE_CODE(bsw){//bsw: blank-separated word
	int in;
	int word_index = 0;
	static char word[0x100];
	while((in = getchar()) > ' '){
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
} NATIVE_ENTRY(quote,"'",nprint);

NATIVE_CODE(see){
	quote_native();
	struct entry *xt = (struct entry*)pop_data();
	if(!xt->code_size){
		puts("Native code.");
		return;
	}
	size_t *code = xt->code.threaded;
	size_t *code_stop = code + xt->code_size;
	for(;code < code_stop; ++code){
		struct entry *current = head;
		for(; current && current != (struct entry*)(*code); current = current->next);
		if(current){
			printf("%s ", current->name);
//			if(&literal_entry == current){
//				printf("%d ",*(++code));
//			}
		}
		else printf("%d ",*code);
	}
	puts("");
} NATIVE_ENTRY(see,"see",quote);

NATIVE_CODE(words){
	struct entry *current = head;
	while(current){
		printf("%s ", current->name);
		current = current->next;
	}
	puts("");
} NATIVE_ENTRY(words,"WORDS",see);

NATIVE_CODE(prompt){
	size_t *ptr = data_stack;
	if(data_ptr - ptr > 5){
		printf("...");
		ptr = data_ptr - 5;
	}
	while(ptr < data_ptr)printf("%d ", *ptr++);
	printf(" >>> ");
} NATIVE_ENTRY(prompt,"PROMPT",words);

NATIVE_CODE(outer_interpreter){
//	prompt_native();//can pass the prompt function on the stack, calling using CALL, to allow a non-interactive session that has an empty prompt function passed in
//	while(1){
//		bsw_native();
//		exec_native();
//		//some way to go back one character, test if it is \n and prompt? How to catch EOF?
//	}
//	return;
	int in;
	int word_index = 0;
	while((in = getchar()) != EOF){
		if(in > ' '){
			word[word_index] = (char)in;
			word_index++;
		}
		else{
			if(word_index){
				word[word_index] = '\0';
				word_index = 0;
				push_data((size_t)word);
				exec_native();
			}
			if('\n' == in)prompt_native();
		}
		if(word_index >= 0x100)error_exit("word length >256");
	}
} NATIVE_ENTRY(outer_interpreter, "OUTER_INTERPRETER", prompt);

NATIVE_CODE(colon){
	int in;
	int word_index = 0;
	if(NULL == temp_code && (code_ptr != code_space))error_exit("first label of code block must be at block start");
	while((in = getchar()) != EOF){
		if(in > ' '){
			word[word_index] = (char)in;
			word_index++;
		}
		else if(word_index){
			word[word_index] = '\0';
			char *new_name = malloc(word_index+1);
			strcpy(new_name, word);
			struct entry *new_word = malloc(sizeof(struct entry));
			new_word->name = new_name;
			new_word->code.threaded = code_ptr;
			new_word->can_delete = (code_ptr == code_space);
			new_word->code_size = 0;
			new_word->next = temp_code;
			temp_code = new_word;
			return;
		}
		if(word_index >= 0x100)error_exit("word length >256");
	}
} NATIVE_ENTRY(colon, ":", outer_interpreter);

NATIVE_CODE(semicolon){
	if((code_ptr == code_space) || ((size_t)&ret_entry != *(code_ptr - 1)))*code_ptr++ = (size_t)&ret_entry;
	size_t global_code_size = (code_ptr - code_space) * (sizeof code_ptr);
	size_t *new_code = malloc(global_code_size);
	memcpy(new_code, code_space, global_code_size);
	while(temp_code){
		struct entry *temp = temp_code;
		temp_code = temp_code->next;
		temp->code_size = code_ptr - temp->code.threaded;
		temp->code.threaded = new_code + (temp->code.threaded - code_space);
		temp->next = head;
		head = temp;
	}
	code_ptr = code_space;
} NATIVE_ENTRY(semicolon, ";", colon);

NATIVE_CODE(compile){
	*code_ptr++ = pop_data();
} NATIVE_ENTRY(compile, ",", semicolon);

NATIVE_CODE(start_threaded_code){
//	size_t *max = code_space + code_ptr;
	static const int delimiter = ']';
	int in;
	int word_index = 0;
	int got_delimiter = 0;
start:
	while((in = getchar()) != EOF){
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
	struct entry *current = head;
	while(current){
		if(!strcmp(word, current->name)){
			*code_ptr++ = (size_t)current;
			break;
		}
		current = current->next;
	}
	if(NULL != current)goto start;
	*code_ptr++ = (size_t)&literal_entry;
	*code_ptr++ = (atoi(word));//XXX TODO temporary, errors give 0
	goto start;	
//	puts("********word not found!");
//	while(1){
//		quote_native();
//		struct entry *xt = (struct entry *)pop_data();
//		if(&end_threaded_code_entry != xt)*code_ptr++ = (size_t)xt;
//		else return;
//	}
} NATIVE_ENTRY(start_threaded_code, "[", compile);

NATIVE_CODE(skip_bl){
	const char delimiter = (char)pop_data();
	int in;
	int word_index = 0;
	int got_delimiter = 0;
	while((in = getchar()) != EOF){
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
} NATIVE_ENTRY(skip_bl, "SKIPBL", start_threaded_code);

NATIVE_CODE(char){
	bsw_native();
	push_data((size_t)(((char *)pop_data())[0]));
} NATIVE_ENTRY(char, "char", skip_bl);

NATIVE_CODE(line_comment){
	int in;
	while((in = getchar()) != EOF){
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
	while((in = getchar()) != EOF){
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
	while((in = getchar()) != EOF){
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

NATIVE_CODE(bool_invert){
	push_data(pop_data() ? 0 : ~0);
} NATIVE_ENTRY(bool_invert, "bool_invert", interactive_if);

struct entry *head = &bool_invert_entry;

int main(void){
	outer_interpreter_native();
	return 0;
}