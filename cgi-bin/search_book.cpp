/*
 * search_book.cpp - 检索书籍是否存在
 */
/* $begin search_book */
//#include "csapp.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define MAXLINE 8704
int main(void) 
{
    char *buf, *p;
    char book[MAXLINE],content[MAXLINE];
	// bool is_find = false;
	const char* huxueyan_url = "\"http://219.216.110.149:8800/file/huxueyan.txt\"";
	const char* guiguzi_url = "\"http://219.216.110.149:8800/file/guiguzi.txt\"";
	// char * url;
    /* Extract book arguments */
    if ((buf = getenv("QUERY_STRING")) != NULL) 
	{
		p = strchr(buf, '=');
		*p = '\0';
		//strcpy(arg1, buf);
		strcpy(book, p+1);
		// n1 = atoi(arg1);
		// n2 = atoi(arg2);
    }
	// // 搜索书籍是否存在
	// if(strstr(book, "huxueyan"))
	// {
		// is_find = true;
	// }
    /* Make the response body */
    sprintf(content, "Welcome to yun tian shu ji: ");
	if(strstr(book, "huxueyan"))
	{
		sprintf(content, "%s<p><a href=%s>huxueyan</a></p>",content,huxueyan_url);
	}
	else if(strstr(book, "guiguzi"))
	{
		sprintf(content, "%s<p><a href=%s>guiguzi</a></p>",content,guiguzi_url);
	}
	else
	{
		sprintf(content, "%s<p>Not found!</p>",content);
	}
    // sprintf(content, "%sTHE Internet addition portal.\r\n<p>", content);
    // sprintf(content, "%sThe answer is: %d + %d = %d\r\n<p>", 
	    // content, n1, n2, n1 + n2);
    sprintf(content, "%s<p>Thanks for visiting!</p>", content);
  
    /* Generate the HTTP response */
    printf("Connection: keep-alive\r\n");
    printf("Content-length: %d\r\n", (int)strlen(content));
    printf("Content-type: text/html\r\n\r\n");
    printf("%s", content);
    fflush(stdout);

    exit(0);
}
/* $end search_book */
