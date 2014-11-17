#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <pcre.h>
#include <liuyx.h>

// file content buff
#define BUF_SIZE 1024

// line buff
#define LINE_BUF 1280

static const char **msplit(const char *string,const char *regex, int limit, int *array_length);
static const char **pattern_split(const char *string, const char *regex, int limit, int *array_length);
static const char **get_regexs(const char *regex_file_path, int *length);
static void write_to_stream(FILE *stream,const char *str,const char *regex_file_path,int (*match_op)(const char *data,const char *regex),bool match_do_stuff);
static int match_with_vector(const char *data,const char *regex,int *ovector,int *rc);

off_t inline get_file_length_p(const char* file_path)
{
	FILE *fp;
	off_t result;
	if ( (fp = fopen(file_path,"r")) == NULL) {
		perror("fopen error");
		exit(1);
	}
	result = get_file_length_f(fp);
	fclose(fp);
	return result;
}

off_t inline get_file_length_f(FILE *fp) {
	struct stat stat_buf;
	if (fstat(fileno(fp),&stat_buf) < 0) {
		perror("stat error");
		exit(1);
	}
	return stat_buf.st_size;
}

/**
 * when file doesn't exist,don't crush
 */
char* file_to_str(const char *file_path)
{
	FILE *fp;
	if ( (fp = fopen(file_path,"r")) == NULL) {
		perror("fopen error");
		return NULL;
	}

	off_t flength = get_file_length_f(fp);

	char *buff = malloc(flength + 1);

	if (buff == NULL)
		return buff;

	fread(buff,sizeof(char),flength,fp);
	buff[flength] = 0;

	if (ferror(fp)){
		free(buff);
		fclose(fp);
		return NULL;
	}
	fclose(fp);

	return buff;
}


void write_to_stream_doesnt_match(FILE *stream,const char *str,const char *regex_file_path,int (*match_op)(const char *data,const char *regex)) {
	write_to_stream(stream,str,regex_file_path,match_op,false);
}

void write_to_stream_match(FILE *stream,const char *str,const char *regex_file_path,int (*match_op)(const char *data,const char *regex)) {
	write_to_stream(stream,str,regex_file_path,match_op,true);
}

int match(const char *data,const char *regex)
{
	static int ovector[OVECCOUNT];
	int rc;
	return match_with_vector(data,regex,ovector,&rc);
}

const char **split(const char *string,const char *regex,int *array_length) {
	return msplit(string,regex,0,array_length);
}

int getgrps(const char *string,const char *regex,...) {
	static int ovector[OVECCOUNT];
	int rc;
	int match = match_with_vector(string,regex,ovector,&rc);
	if (match == MATCH) {
		va_list ap;
		va_start(ap,regex);

		char *grp_arg;
		const char *grp;
		int grp_len;

		int i;
		for (i = 1; i < rc; i++) {
			grp_arg = va_arg(ap,char *);
			grp = string + ovector[2 * i];
			grp_len = ovector[2 * i + 1] - ovector[2 * i];
			sprintf(grp_arg,"%.*s",grp_len,grp);
		}

		va_end(ap);
	}
	return match;
}

//---------------------------------help functions--------------------------------------

static const char ** msplit(const char *string,const char *regex,int limit,int *array_length) {
	/* fastpath if the regex is a
	 * (1) one-char String and this character is not one of the
	 * RegEx's meta characters ".$|()[{^?*+\\",or
	 * (2) two-char String and the first char is the backslask and
	 * the second is not the ascii digit or ascii letter.
	 */
	char ch = 0;
	size_t str_len = strlen(string);
	size_t reg_len = strlen(regex);
	if (reg_len == 1 && 
		strstr(".$|()[{^?*+\\",regex) == NULL ||
		(reg_len == 2 &&
		 regex[0] == '\\' &&
		 (((ch = regex[1]) - '0') | ('9' - ch)) < 0 &&
		 ((ch - 'a') | ('z' - ch)) < 0 &&
		 ((ch - 'A') | ('Z' - ch)) < 0))
	{
		//char **array = malloc(BUF_SIZE * sizeof(char *));
		char **array = malloc(BUF_SIZE * sizeof(char *));
		int array_size = 0;
		int start = 0;
		int i;
		char lim = regex[0];
		for (i = 0; i < str_len; i++) {
			if (string[i] == lim) {
				char *substring = strndup(string + start, i - start);
				if (strcmp(substring,"") != 0)
					array[array_size++] = substring;
				else
					free(substring);
				
				//if (memcmp(string + start, "",i - start) != 0){
				//	array[array_size++] = malloc(i - start + 1);
				//	memcpy(*array,string + start, i - start);
				//}
				start = i + 1;
			}
		}
		if (start != str_len)
			//memcpy(array[array_size++], string + start, str_len - start);
			array[array_size++] = strndup(string + start, str_len - start);
		const char **result = malloc(array_size * sizeof(char *));
		memcpy(result,array,array_size * sizeof(char *));
		*array_length = array_size;
		free(array);
		return result;
	}

	return pattern_split(string,regex,limit,array_length);
}


static const char **pattern_split(const char *string,const char *regex,int limit,int *array_length) {
	int index = 0;
	bool matchLimited = limit > 0;
	char **list = malloc(sizeof(char *) * BUF_SIZE);
	int ovector[OVECCOUNT] = {0};
	int rc;
	int match_state = match_with_vector(string,regex,ovector,&rc);
	if (match_state == NOMATCH){
		free(list);
		return NULL;
	}
	int match_list_size = 0;

	int last_end = 0;

	while (1) {
		int start = ovector[0] + last_end;
		int end = ovector[1] + last_end;
		last_end = end;
		if (!matchLimited || match_list_size < limit - 1) {
			if (index == 0 && index == start && start == end) {
				// no empty leading substring included for zero-width match
				// at the beginning of the input char sequence.
				goto again;
			}
			
			list[match_list_size++] = strndup(string + index, start - index);	
			index = end;
		} else if (match_list_size == limit - 1) { // last one
			list[match_list_size++] = strndup(string + index, strlen(string) - index);
			index = end;
		}
	again:
		bzero(ovector,sizeof(ovector));
		match_state = match_with_vector(string + end,regex,ovector,&rc);
		if (match_state == NOMATCH) break;
	}

	// If no match was found, return this
	if (index == 0) {
		const char **result = malloc(sizeof(char *));
		*result = string;
		*array_length = 1;
		free(list);
		return result;
	}

	// Add remaining segment
	if (!matchLimited || match_list_size < limit) {
		list[match_list_size++] = strndup(string + index, strlen(string) - index);
	}

	// Construct result
	int resultSize = match_list_size;
	if (limit == 0)
		while (resultSize > 0 && strcmp(list[resultSize - 1],"") == 0)
			resultSize--;
	const char **result = malloc((resultSize) * sizeof(char *));
	memcpy(result,list,(resultSize) * sizeof(char *));
	free(list);
	*array_length = resultSize;
	return result;
}


static const char **get_regexs(const char *regex_file_path,int *length) {
	char *text = file_to_str(regex_file_path);
	if (text == NULL) return NULL;
	int array_len;
	const char **strings = split(text,"\n", &array_len);
	const char **regex = malloc(array_len * sizeof(char *));
	free(text);
	int i;
	int regex_len = 0;
	for (i = 0; i < array_len; i++) {
		const char *string = strings[i];
		if (match(string,"(?:^\\s*$)|(?:^\\s*#)") == NOMATCH)
			regex[regex_len++] = string;
		else
			free((void *)string);
	}
	free(strings);
	*length = regex_len;
	return regex;
}

/* uniq line command */
#define UNIQ_LINE_CMD "^\\s*{\\s*uniq\\s+(\\d+)?\\s*}\\s*$"

/* timeout command */
#define TIME_OUT_CMD  "^\\s*{\\s*timeout\\s+(\\d+)?\\s*}\\s*$"


static void inline get_num_from_cmd(const char *regex, const char *cmd,bool *flag,int *num) {
	char buff[10] = {0};
	if (getgrps(regex,cmd,buff) == MATCH) {
		*flag = true;
		if (strcmp(buff,"") == 0) {
			*num = 1;
		} else {
			*num = atoi(buff);
		}
	}
}

static void write_to_stream(FILE *stream,const char *str,const char *regex_file_path,int (*match_op)(const char *data,const char *regex),bool match_do_stuff) {
	int regex_len       = 0;
	const char **regexs = get_regexs(regex_file_path,&regex_len);
	int match           = NOMATCH;
	bool uniq_op        = false;
	bool time_out_op    = false;
	int uniq_line       = 0;
	int time_out        = 0;

	if (regexs == NULL || match_op == NULL) {
		if (!match_do_stuff)
			fwrite(str,strlen(str),sizeof(char),stream);
		return;
	}

	int i;
	for (i = 0; i < regex_len; i++) {
		char num_buf[10] = {0};

		/* is there uniq command ? */
		get_num_from_cmd(regexs[i],UNIQ_LINE_CMD,&uniq_op,&uniq_line);

		/* is there timeout command ? */
		get_num_from_cmd(regexs[i],TIME_OUT_CMD,&time_out_op,&time_out);

		match |= match_op(str,regexs[i]);
		free((void *)regexs[i]);
	}

	free(regexs);

	bool not_write_dup = false;
	size_t str_len = strlen(str);


	static char **line_rindex_buff;


	if (uniq_op) {
		if (line_rindex_buff == NULL) {
			line_rindex_buff = malloc(10 * sizeof(char *));

			for (i = 0; i < 10; i++) {
				line_rindex_buff[i] = NULL;
			}
		}

		for (i = 0; i < uniq_line; i++) {
			if (line_rindex_buff[i] == NULL)
				line_rindex_buff[i] = malloc(LINE_BUF);
		}

		for (i = 0; i < uniq_line; i++) {
			if (memcmp(line_rindex_buff[i],str,str_len + 1) == 0) {
				not_write_dup = true;
				break;
			}
		}

		/* iterate copy: last_line => line_r2, line_r2 => line_r3, line_r3 => line_r4 ...line_r$(uniq_line-1) => line_r$(uniq_line) */
		for (i = uniq_line - 1; i - 1 >= 0; i--) {
			memcpy(line_rindex_buff[i], line_rindex_buff[i - 1], strlen(line_rindex_buff[i - 1]) + 1);
		}
		memcpy(line_rindex_buff[0],str,str_len + 1);

		for (i = uniq_line; i < 10; i++) {
			if (line_rindex_buff[i] != NULL)
				free(line_rindex_buff[i]);
		}
		//free(line_rindex_buff);
	}

	if (match_do_stuff) {

		/**
		 * when match & filter is true, write to the stream
		 */
		if (match && !not_write_dup) {
			goto time;
			match_back:	fwrite(str,strlen(str),sizeof(char),stream);
		}
	} else {

		/**
		 * when don't match but don't filter, write to the stream
		 */
		if (!match && !not_write_dup) {
			goto time;
			not_match_back: fwrite(str,strlen(str),sizeof(char),stream);
		}
		return;
	}

time:
	/* operate the time_out */
	if (time_out_op) {
		static time_t last_time = 0;

		time_t current_time = time(NULL);
		if (current_time - last_time > time_out) {
			// empty the file
			fseek(stream,0,SEEK_SET);
			ftruncate(fileno(stream),0);
		}

		last_time = current_time;
	}

	if (match)
		goto match_back;
	else
		goto not_match_back;
}

static int match_with_vector(const char *data,const char *regex,int *ovector,int *rc) {
	pcre *re;
	const char *error;
	int erroffset;

	re = pcre_compile(
			regex,				/* the pattern */
			0,					/* default options */
			&error,				/* for error message */
			&erroffset,			/* for error offset */
			NULL);				/* use default character tables */
	if (!re) {
		fprintf(stderr,"regex compile error\n");
		exit(1);
	}

	*rc = pcre_exec(
			re,					/* the compiled pattern */
			NULL,				/* no extra data - we didn't study the pattern */
			data,				/* the subject string */
			strlen(data),		/* the length of the subject */ 
			0,					/* start at offset 0 in the subject */
			0,					/* default options */
			ovector,			/* output vector for substring information */
			OVECCOUNT);			/* number of elements in the output vector */ 
	free(re);
	if (*rc < 0) {
		switch (*rc) {
			case PCRE_ERROR_NOMATCH:
				break;

			/* More cases defined */
			default:
				break;
		}
		//free(rc);
		return NOMATCH;
	}
	//free(rc);
	return MATCH;
}
