#include "stdafx.h"

static int global_time_gap = 0;

time_t get_global_time()
{
	return time(0) + global_time_gap;
}

void set_global_time(time_t t)
{
	global_time_gap = t - time(0);

	char time_str_buf[32];
	snprintf(time_str_buf, sizeof(time_str_buf), "%s", time_str(get_global_time()));

	SPDLOG_INFO("GLOBAL_TIME: {} time_gap {}", time_str_buf, global_time_gap);
}

size_t str_lower(const char * src, char * dest, size_t dest_size)
{
	size_t len = 0;

	if (!dest || dest_size == 0)
		return len;

	if (!src)
	{
		*dest = '\0';
		return len;
	}

	// \0 �ڸ� Ȯ��
	--dest_size;

	while (*src && len < dest_size)
	{
		*dest = LOWER(*src); // LOWER ��ũ�ο��� ++�� --�ϸ� �ȵ�!!

		++src;
		++dest;
		++len;
	}

	*dest = '\0';
	return len;
}

void skip_spaces(const char **string)
{   
	for (; **string != '\0' && isspace(**string); ++(*string));
}

const char *one_argument(const char *argument, char *first_arg, size_t first_size)
{
	bool mark = false;
	size_t first_len = 0;

	if (!argument || 0 == first_size)
	{
		SPDLOG_ERROR("one_argument received a NULL pointer!");
		*first_arg = '\0';
		return NULL;    
	} 

	// \0 �ڸ� Ȯ��
	--first_size;

	skip_spaces(&argument);

	while (*argument && first_len < first_size)
	{ 
		if (*argument == '\"')
		{
			mark = !mark;
			++argument; 
			continue;   
		}

		if (!mark && isspace(*argument))
			break;

		*(first_arg++) = *argument;
		++argument;     
		++first_len;
	} 

	*first_arg = '\0';

	skip_spaces(&argument);
	return (argument);
}

const char *two_arguments(const char *argument, char *first_arg, size_t first_size, char *second_arg, size_t second_size)
{
	return (one_argument(one_argument(argument, first_arg, first_size), second_arg, second_size));
}

const char *first_cmd(const char *argument, char *first_arg, size_t first_arg_size, size_t *first_arg_len_result)
{           
	size_t cur_len = 0;
	skip_spaces(&argument);

	// \0 �ڸ� Ȯ��
	first_arg_size -= 1;

	while (*argument && !isspace(*argument) && cur_len < first_arg_size)
	{
		*(first_arg++) = LOWER(*argument);
		++argument;
		++cur_len;
	}

	*first_arg_len_result = cur_len;
	*first_arg = '\0';
	return (argument);
}

int CalculateDuration(int iSpd, int iDur)
{
	int i = 100 - iSpd;

	if (i > 0) 
		i = 100 + i;
	else if (i < 0) 
		i = 10000 / (100 - i);
	else
		i = 100;

	return iDur * i / 100;
}

int parse_time_str(const char* str)
{
	int tmp = 0;
	int secs = 0;

	while (*str != 0)
	{
		switch (*str)
		{
			case 'm':
			case 'M':
				secs += tmp * 60;
				tmp = 0;
				break;

			case 'h':
			case 'H':
				secs += tmp * 3600;
				tmp = 0;
				break;

			case 'd':
			case 'D':
				secs += tmp * 86400;
				tmp = 0;
				break;

			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
				tmp *= 10;
				tmp += (*str) - '0';
				break;

			case 's':
			case 'S':
				secs += tmp;
				tmp = 0;
				break;
			default:
				return -1;
		}
		++str;
	}

	return secs + tmp;
}

bool WildCaseCmp(const char *w, const char *s)
{
	for (;;)
	{
		switch(*w)
		{
			case '*':
				if ('\0' == w[1])
					return true;
				{
					for (size_t i = 0; i <= strlen(s); ++i)
					{
						if (true == WildCaseCmp(w + 1, s + i))
							return true;
					}
				}
				return false;

			case '?':
				if ('\0' == *s)
					return false;

				++w;
				++s;
				break;

			default:
				if (*w != *s)
				{
					if (tolower(*w) != tolower(*s))
						return false;
				}

				if ('\0' == *w)
					return true;

				++w;
				++s;
				break;
		}
	}

	return false;
}

