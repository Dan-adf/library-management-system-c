#include "app.h"

#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_SERVER_IP "127.0.0.1"
#define DEFAULT_SERVER_PORT 18080
#define BUFFER_SIZE 4096
#define NAME_SIZE 64
#define ROLE_SIZE 16

static int send_all(
    int socket_fd,
    const char *data,
    size_t length
)
{
    size_t sent_total = 0;

    while (sent_total < length) {
        ssize_t sent = send(
            socket_fd,
            data + sent_total,
            length - sent_total,
            0
        );

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (sent == 0) {
            return -1;
        }

        sent_total += (size_t)sent;
    }

    return 0;
}

static int send_line(
    int socket_fd,
    const char *message
)
{
    if (send_all(
            socket_fd,
            message,
            strlen(message)) == -1) {
        return -1;
    }

    return send_all(socket_fd, "\n", 1);
}

static ssize_t recv_line(
    int socket_fd,
    char *buffer,
    size_t capacity
)
{
    size_t position = 0;
    int too_long = 0;

    if (capacity < 2) {
        return -1;
    }

    while (1) {
        char ch;

        ssize_t received = recv(
            socket_fd,
            &ch,
            1,
            0
        );

        if (received == 0) {
            if (position == 0) {
                return 0;
            }

            break;
        }

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            return -1;
        }

        if (ch == '\n') {
            break;
        }

        if (ch == '\r') {
            continue;
        }

        if (position < capacity - 1) {
            buffer[position++] = ch;
        } else {
            too_long = 1;
        }
    }

    buffer[position] = '\0';

    if (too_long) {
        return -2;
    }

    return (ssize_t)position;
}

/* 安全读取一行用户输入 */
static int read_input(
    const char *prompt,
    char *buffer,
    size_t capacity
)
{
    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buffer, (int)capacity, stdin) == NULL) {
        return 0;
    }

    char *newline = strchr(buffer, '\n');

    if (newline != NULL) {
        *newline = '\0';
    } else {
        /* 输入过长时清理剩余字符 */
        int ch;

        while ((ch = getchar()) != '\n' && ch != EOF) {
        }
    }

    size_t length = strlen(buffer);

    if (length > 0 && buffer[length - 1] == '\r') {
        buffer[length - 1] = '\0';
    }

    return 1;
}

/*
 * 创建通用 JSON 请求。
 * data 的所有权会转交给 request。
 * 返回值必须使用 cJSON_free() 释放。
 */
static char *create_request(
    const char *action,
    const char *request_id,
    cJSON *data
)
{
    cJSON *request = cJSON_CreateObject();

    if (request == NULL) {
        cJSON_Delete(data);
        return NULL;
    }

    cJSON_AddStringToObject(
        request,
        "action",
        action
    );

    cJSON_AddStringToObject(
        request,
        "request_id",
        request_id
    );

    if (data != NULL) {
        cJSON_AddItemToObject(
            request,
            "data",
            data
        );
    } else {
        cJSON_AddObjectToObject(
            request,
            "data"
        );
    }

    char *request_text =
        cJSON_PrintUnformatted(request);

    cJSON_Delete(request);
    return request_text;
}

/*
 * 发送 JSON 请求并接收 JSON 响应。
 * request 会在本函数内释放。
 */
static int exchange_json(
    int socket_fd,
    char *request,
    char *response,
    size_t response_capacity
)
{
    if (request == NULL) {
        fprintf(stderr, "Failed to create JSON request.\n");
        return 0;
    }

    cJSON *request_summary = cJSON_Parse(request);
    cJSON *summary_action = cJSON_IsObject(request_summary)
        ? cJSON_GetObjectItemCaseSensitive(request_summary, "action")
        : NULL;
    cJSON *summary_id = cJSON_IsObject(request_summary)
        ? cJSON_GetObjectItemCaseSensitive(request_summary, "request_id")
        : NULL;
    printf(
        "\nRequest: action=%s, request_id=%s\n",
        cJSON_IsString(summary_action)
            ? summary_action->valuestring : "INVALID",
        cJSON_IsString(summary_id)
            ? summary_id->valuestring : "-"
    );
    cJSON_Delete(request_summary);

    if (send_line(socket_fd, request) == -1) {
        perror("send");
        cJSON_free(request);
        return 0;
    }

    cJSON_free(request);

    ssize_t received = recv_line(
        socket_fd,
        response,
        response_capacity
    );

    if (received == 0) {
        printf("Server disconnected.\n");
        return 0;
    }

    if (received == -1) {
        perror("recv");
        return 0;
    }

    if (received == -2) {
        fprintf(stderr, "Server response is too long.\n");
        return 0;
    }

    printf("Response: %s\n", response);
    return 1;
}

/*
 * 解析并显示响应。
 *
 * 如果响应中包含登录用户信息，会复制 name 和 role。
 * 返回 1 表示业务操作成功，返回 0 表示失败。
 */
static int parse_response(
    const char *response_text,
    char *name,
    size_t name_capacity,
    char *role,
    size_t role_capacity
)
{
    cJSON *response =
        cJSON_Parse(response_text);

    if (!cJSON_IsObject(response)) {
        fprintf(stderr, "Server returned invalid JSON.\n");
        cJSON_Delete(response);
        return 0;
    }

    cJSON *success =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "success"
        );

    cJSON *message =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "message"
        );

    int operation_success =
        cJSON_IsTrue(success);

    printf(
        "Result: %s\n",
        operation_success ? "success" : "failed"
    );

    if (cJSON_IsString(message)) {
        printf(
            "Message: %s\n",
            message->valuestring
        );
    }

    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "data"
        );

    if (cJSON_IsObject(data)) {
        cJSON *user_id =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "user_id"
            );

        cJSON *username =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "username"
            );

        cJSON *user_name =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "name"
            );

        cJSON *user_role =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "role"
            );

        cJSON *echo =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "echo"
            );

        if (cJSON_IsNumber(user_id)) {
            printf("User ID: %d\n", user_id->valueint);
        }

        if (cJSON_IsString(username)) {
            printf(
                "Username: %s\n",
                username->valuestring
            );
        }

        if (cJSON_IsString(user_name)) {
            printf(
                "Name: %s\n",
                user_name->valuestring
            );

            if (name != NULL && name_capacity > 0) {
                snprintf(
                    name,
                    name_capacity,
                    "%s",
                    user_name->valuestring
                );
            }
        }

        if (cJSON_IsString(user_role)) {
            printf(
                "Role: %s\n",
                user_role->valuestring
            );

            if (role != NULL && role_capacity > 0) {
                snprintf(
                    role,
                    role_capacity,
                    "%s",
                    user_role->valuestring
                );
            }
        }

        if (cJSON_IsString(echo)) {
            printf("Echo: %s\n", echo->valuestring);
        }
    }

    cJSON_Delete(response);
    return operation_success;
}

static void create_request_id(
    char *buffer,
    size_t capacity,
    unsigned int *request_number
)
{
    snprintf(
        buffer,
        capacity,
        "REQ-%u",
        (*request_number)++
    );
}

static int perform_login(
    int socket_fd,
    unsigned int *request_number,
    char *current_name,
    size_t name_capacity,
    char *current_role,
    size_t role_capacity
)
{
    char username[64];
    char password[64];
    char request_id[32];
    char response[BUFFER_SIZE];

    if (!read_input(
            "Username: ",
            username,
            sizeof(username))) {
        return 0;
    }

    if (!read_input(
            "Password: ",
            password,
            sizeof(password))) {
        return 0;
    }

    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return 0;
    }

    cJSON_AddStringToObject(
        data,
        "username",
        username
    );

    cJSON_AddStringToObject(
        data,
        "password",
        password
    );

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "LOGIN",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response,
            sizeof(response))) {
        return 0;
    }

    return parse_response(
        response,
        current_name,
        name_capacity,
        current_role,
        role_capacity
    );
}

static int perform_simple_action(
    int socket_fd,
    const char *action,
    unsigned int *request_number
)
{
    char request_id[32];
    char response[BUFFER_SIZE];

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        action,
        request_id,
        NULL
    );

    if (!exchange_json(
            socket_fd,
            request,
            response,
            sizeof(response))) {
        return 0;
    }

    return parse_response(
        response,
        NULL,
        0,
        NULL,
        0
    );
}

static int perform_ping(
    int socket_fd,
    unsigned int *request_number
)
{
    char message[256];
    char request_id[32];
    char response[BUFFER_SIZE];

    if (!read_input(
            "Message: ",
            message,
            sizeof(message))) {
        return 0;
    }

    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return 0;
    }

    cJSON_AddStringToObject(
        data,
        "message",
        message
    );

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "PING",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response,
            sizeof(response))) {
        return 0;
    }

    return parse_response(
        response,
        NULL,
        0,
        NULL,
        0
    );
}

static const char *get_json_string(
    const cJSON *object,
    const char *key
)
{
    cJSON *item =
        cJSON_GetObjectItemCaseSensitive(
            object,
            key
        );

    if (!cJSON_IsString(item)) {
        return "";
    }

    return item->valuestring;
}

static int get_json_int(
    const cJSON *object,
    const char *key
)
{
    cJSON *item =
        cJSON_GetObjectItemCaseSensitive(
            object,
            key
        );

    if (!cJSON_IsNumber(item)) {
        return 0;
    }

    return item->valueint;
}

static int print_books_response(
    const char *response_text
)
{
    cJSON *response =
        cJSON_Parse(response_text);

    if (!cJSON_IsObject(response)) {
        fprintf(
            stderr,
            "Server returned invalid JSON.\n"
        );

        cJSON_Delete(response);
        return 0;
    }

    cJSON *success =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "success"
        );

    cJSON *message =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "message"
        );

    if (!cJSON_IsTrue(success)) {
        if (cJSON_IsString(message)) {
            printf(
                "Operation failed: %s\n",
                message->valuestring
            );
        }

        cJSON_Delete(response);
        return 0;
    }

    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "data"
        );

    cJSON *books = NULL;

    if (cJSON_IsObject(data)) {
        books =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "books"
            );
    }

    if (!cJSON_IsArray(books)) {
        fprintf(
            stderr,
            "Response does not contain books.\n"
        );

        cJSON_Delete(response);
        return 0;
    }

    int count = cJSON_GetArraySize(books);

    printf("\n========== Book List ==========\n");
    printf("Found: %d\n", count);

    if (count == 0) {
        printf("No matching books.\n");
    }

    cJSON *book = NULL;
    int number = 1;

    cJSON_ArrayForEach(book, books) {
        printf("\n[%d]\n", number++);

        printf(
            "ID: %d\n",
            get_json_int(book, "id")
        );

        printf(
            "Title: %s\n",
            get_json_string(book, "title")
        );

        printf(
            "Author: %s\n",
            get_json_string(book, "author")
        );

        printf(
            "ISBN: %s\n",
            get_json_string(book, "isbn")
        );

        printf(
            "Publisher: %s\n",
            get_json_string(book, "publisher")
        );

        printf(
            "Category: %s\n",
            get_json_string(book, "category")
        );

        printf(
            "Stock: %d/%d\n",
            get_json_int(book, "available_count"),
            get_json_int(book, "total_count")
        );

        printf(
            "Location: %s\n",
            get_json_string(book, "location")
        );

        printf(
            "Status: %s\n",
            get_json_string(book, "status")
        );
    }

    printf("\n===============================\n");

    cJSON_Delete(response);
    return 1;
}

static int perform_list_books(
    int socket_fd,
    unsigned int *request_number
)
{
    char request_id[32];
    char response[BUFFER_SIZE];

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "LIST_BOOKS",
        request_id,
        NULL
    );

    if (!exchange_json(
            socket_fd,
            request,
            response,
            sizeof(response))) {
        return 0;
    }

    return print_books_response(response);
}

static int perform_search_books(
    int socket_fd,
    unsigned int *request_number
)
{
    char keyword[128];
    char request_id[32];
    char response[BUFFER_SIZE];

    if (!read_input(
            "Search keyword: ",
            keyword,
            sizeof(keyword))) {
        return 0;
    }

    if (keyword[0] == '\0') {
        printf("Keyword cannot be empty.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return 0;
    }

    cJSON_AddStringToObject(
        data,
        "keyword",
        keyword
    );

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "SEARCH_BOOKS",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response,
            sizeof(response))) {
        return 0;
    }

    return print_books_response(response);
}

static int perform_borrow_book(
    int socket_fd,
    unsigned int *request_number
)
{
    char input[32];
    char request_id[32];
    char response_text[BUFFER_SIZE];

    if (!read_input(
            "Book ID: ",
            input,
            sizeof(input))) {
        return 0;
    }

    char *end = NULL;
    errno = 0;

    long book_id = strtol(
        input,
        &end,
        10
    );

    if (errno != 0 ||
        end == input ||
        *end != '\0' ||
        book_id <= 0 ||
        book_id > 2147483647L) {
        printf("Invalid book ID.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return 0;
    }

    cJSON_AddNumberToObject(
        data,
        "book_id",
        (double)book_id
    );

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "BORROW_BOOK",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    int success = parse_response(
        response_text,
        NULL,
        0,
        NULL,
        0
    );

    if (!success) {
        return 0;
    }

    cJSON *response =
        cJSON_Parse(response_text);

    if (cJSON_IsObject(response)) {
        cJSON *response_data =
            cJSON_GetObjectItemCaseSensitive(
                response,
                "data"
            );

        if (cJSON_IsObject(response_data)) {
            cJSON *record_id =
                cJSON_GetObjectItemCaseSensitive(
                    response_data,
                    "record_id"
                );

            cJSON *title =
                cJSON_GetObjectItemCaseSensitive(
                    response_data,
                    "title"
                );

            cJSON *due_time =
                cJSON_GetObjectItemCaseSensitive(
                    response_data,
                    "due_time"
                );

            if (cJSON_IsNumber(record_id)) {
                printf(
                    "Record ID: %d\n",
                    record_id->valueint
                );
            }

            if (cJSON_IsString(title)) {
                printf(
                    "Book: %s\n",
                    title->valuestring
                );
            }

            if (cJSON_IsString(due_time)) {
                printf(
                    "Due time: %s\n",
                    due_time->valuestring
                );
            }
        }
    }

    cJSON_Delete(response);
    return 1;
}

static int perform_my_borrows(
    int socket_fd,
    unsigned int *request_number
)
{
    char request_id[32];
    char response_text[BUFFER_SIZE];

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "MY_BORROWS",
        request_id,
        NULL
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    cJSON *response =
        cJSON_Parse(response_text);

    if (!cJSON_IsObject(response)) {
        fprintf(
            stderr,
            "Server returned invalid JSON.\n"
        );

        cJSON_Delete(response);
        return 0;
    }

    cJSON *success =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "success"
        );

    cJSON *message =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "message"
        );

    if (!cJSON_IsTrue(success)) {
        if (cJSON_IsString(message)) {
            printf(
                "Operation failed: %s\n",
                message->valuestring
            );
        }

        cJSON_Delete(response);
        return 0;
    }

    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "data"
        );

    cJSON *records = NULL;

    if (cJSON_IsObject(data)) {
        records =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "records"
            );
    }

    if (!cJSON_IsArray(records)) {
        fprintf(
            stderr,
            "Response does not contain borrow records.\n"
        );

        cJSON_Delete(response);
        return 0;
    }

    int count = cJSON_GetArraySize(records);

    printf("\n======= My Borrow Records =======\n");
    printf("Total: %d\n", count);

    if (count == 0) {
        printf("No borrow records.\n");
    }

    cJSON *record = NULL;
    int number = 1;

    cJSON_ArrayForEach(record, records) {
        const char *return_time =
            get_json_string(
                record,
                "return_time"
            );

        printf("\n[%d]\n", number++);

        printf(
            "Record ID: %d\n",
            get_json_int(record, "record_id")
        );

        printf(
            "Book ID: %d\n",
            get_json_int(record, "book_id")
        );

        printf(
            "Title: %s\n",
            get_json_string(record, "title")
        );

        printf(
            "Borrow time: %s\n",
            get_json_string(record, "borrow_time")
        );

        printf(
            "Due time: %s\n",
            get_json_string(record, "due_time")
        );

        printf(
            "Return time: %s\n",
            return_time[0] == '\0'
                ? "-"
                : return_time
        );

        printf(
            "Renew count: %d\n",
            get_json_int(record, "renew_count")
        );

        printf(
            "Status: %s\n",
            get_json_string(record, "status")
        );
    }

    printf("\n=================================\n");

    cJSON_Delete(response);
    return 1;
}

static int perform_return_book(
    int socket_fd,
    unsigned int *request_number
)
{
    char input[32];
    char request_id[32];
    char response_text[BUFFER_SIZE];

    if (!read_input(
            "Borrow record ID: ",
            input,
            sizeof(input))) {
        return 0;
    }

    char *end = NULL;
    errno = 0;

    long record_id = strtol(
        input,
        &end,
        10
    );

    if (errno != 0 ||
        end == input ||
        *end != '\0' ||
        record_id <= 0 ||
        record_id > 2147483647L) {
        printf("Invalid record ID.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return 0;
    }

    cJSON_AddNumberToObject(
        data,
        "record_id",
        (double)record_id
    );

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "RETURN_BOOK",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    return parse_response(
        response_text,
        NULL,
        0,
        NULL,
        0
    );
}


static int perform_add_book(
    int socket_fd,
    unsigned int *request_number
)
{
    char isbn[32];
    char title[128];
    char author[128];
    char publisher[128];
    char category[64];
    char location[64];
    char count_text[32];
    char request_id[32];
    char response_text[BUFFER_SIZE];

    if (!read_input("ISBN: ", isbn, sizeof(isbn)) ||
        !read_input("Title: ", title, sizeof(title)) ||
        !read_input("Author: ", author, sizeof(author)) ||
        !read_input(
            "Publisher: ",
            publisher,
            sizeof(publisher)) ||
        !read_input(
            "Category: ",
            category,
            sizeof(category)) ||
        !read_input(
            "Location: ",
            location,
            sizeof(location)) ||
        !read_input(
            "Total count: ",
            count_text,
            sizeof(count_text))) {
        return 0;
    }

    char *end = NULL;
    errno = 0;

    long total_count = strtol(
        count_text,
        &end,
        10
    );

    if (errno != 0 ||
        end == count_text ||
        *end != '\0' ||
        total_count <= 0 ||
        total_count > 100000) {
        printf("Invalid total count.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return 0;
    }

    cJSON_AddStringToObject(data, "isbn", isbn);
    cJSON_AddStringToObject(data, "title", title);
    cJSON_AddStringToObject(data, "author", author);
    cJSON_AddStringToObject(
        data,
        "publisher",
        publisher
    );
    cJSON_AddStringToObject(
        data,
        "category",
        category
    );
    cJSON_AddStringToObject(
        data,
        "location",
        location
    );
    cJSON_AddNumberToObject(
        data,
        "total_count",
        (double)total_count
    );

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "ADD_BOOK",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    return parse_response(
        response_text,
        NULL,
        0,
        NULL,
        0
    );
}

static int perform_add_user(
    int socket_fd,
    unsigned int *request_number
)
{
    char username[64];
    char password[129];
    char name[64];
    char request_id[32];
    char response_text[BUFFER_SIZE];

    if (!read_input(
            "New username: ",
            username,
            sizeof(username)) ||
        !read_input(
            "New password: ",
            password,
            sizeof(password)) ||
        !read_input(
            "Reader name: ",
            name,
            sizeof(name))) {
        return 0;
    }

    if (username[0] == '\0' ||
        password[0] == '\0' ||
        name[0] == '\0') {
        printf("Fields cannot be empty.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return 0;
    }

    cJSON_AddStringToObject(
        data,
        "username",
        username
    );

    cJSON_AddStringToObject(
        data,
        "password",
        password
    );

    cJSON_AddStringToObject(
        data,
        "name",
        name
    );

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "ADD_USER",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    return parse_response(
        response_text,
        NULL,
        0,
        NULL,
        0
    );
}

static int perform_all_borrows(
    int socket_fd,
    unsigned int *request_number
)
{
    char request_id[32];
    char response_text[BUFFER_SIZE];

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "ALL_BORROWS",
        request_id,
        NULL
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    cJSON *response =
        cJSON_Parse(response_text);

    if (!cJSON_IsObject(response)) {
        fprintf(
            stderr,
            "Server returned invalid JSON.\n"
        );

        cJSON_Delete(response);
        return 0;
    }

    cJSON *success =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "success"
        );

    cJSON *message =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "message"
        );

    if (!cJSON_IsTrue(success)) {
        if (cJSON_IsString(message)) {
            printf(
                "Operation failed: %s\n",
                message->valuestring
            );
        }

        cJSON_Delete(response);
        return 0;
    }

    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "data"
        );

    cJSON *records = NULL;

    if (cJSON_IsObject(data)) {
        records =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "records"
            );
    }

    if (!cJSON_IsArray(records)) {
        fprintf(
            stderr,
            "Response does not contain records.\n"
        );

        cJSON_Delete(response);
        return 0;
    }

    printf("\n======= All Borrow Records =======\n");
    printf(
        "Total: %d\n",
        cJSON_GetArraySize(records)
    );

    cJSON *record = NULL;
    int number = 1;

    cJSON_ArrayForEach(record, records) {
        const char *return_time =
            get_json_string(
                record,
                "return_time"
            );

        printf("\n[%d]\n", number++);

        printf(
            "Record ID: %d\n",
            get_json_int(record, "record_id")
        );

        printf(
            "User: %s (%s)\n",
            get_json_string(record, "reader_name"),
            get_json_string(record, "username")
        );

        printf(
            "User ID: %d\n",
            get_json_int(record, "user_id")
        );

        printf(
            "Book: %s\n",
            get_json_string(record, "title")
        );

        printf(
            "Book ID: %d\n",
            get_json_int(record, "book_id")
        );

        printf(
            "Borrow time: %s\n",
            get_json_string(record, "borrow_time")
        );

        printf(
            "Due time: %s\n",
            get_json_string(record, "due_time")
        );

        printf(
            "Return time: %s\n",
            return_time[0] == '\0'
                ? "-"
                : return_time
        );

        printf(
            "Status: %s\n",
            get_json_string(record, "status")
        );
    }

    printf("\n==================================\n");

    cJSON_Delete(response);
    return 1;
}

static int perform_list_users(
    int socket_fd,
    unsigned int *request_number
)
{
    char request_id[32];
    char response_text[BUFFER_SIZE];

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "LIST_USERS",
        request_id,
        NULL
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    cJSON *response =
        cJSON_Parse(response_text);

    if (!cJSON_IsObject(response)) {
        cJSON_Delete(response);
        printf("Invalid server response.\n");
        return 0;
    }

    cJSON *success =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "success"
        );

    if (!cJSON_IsTrue(success)) {
        cJSON *message =
            cJSON_GetObjectItemCaseSensitive(
                response,
                "message"
            );

        if (cJSON_IsString(message)) {
            printf(
                "Operation failed: %s\n",
                message->valuestring
            );
        }

        cJSON_Delete(response);
        return 0;
    }

    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(
            response,
            "data"
        );

    cJSON *users = NULL;

    if (cJSON_IsObject(data)) {
        users =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "users"
            );
    }

    if (!cJSON_IsArray(users)) {
        cJSON_Delete(response);
        printf("Response does not contain users.\n");
        return 0;
    }

    printf("\n========== User List ==========\n");

    cJSON *user = NULL;

    cJSON_ArrayForEach(user, users) {
        cJSON *enabled =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "enabled"
            );

        printf(
            "\nID: %d\n",
            get_json_int(user, "id")
        );

        printf(
            "Username: %s\n",
            get_json_string(user, "username")
        );

        printf(
            "Name: %s\n",
            get_json_string(user, "name")
        );

        printf(
            "Role: %s\n",
            get_json_string(user, "role")
        );

        printf(
            "Status: %s\n",
            cJSON_IsTrue(enabled)
                ? "ENABLED"
                : "DISABLED"
        );
    }

    printf("\n===============================\n");

    cJSON_Delete(response);
    return 1;
}

static int perform_set_user_status(
    int socket_fd,
    unsigned int *request_number
)
{
    char user_id_text[32];
    char status_text[16];
    char request_id[32];
    char response_text[BUFFER_SIZE];

    if (!read_input(
            "User ID: ",
            user_id_text,
            sizeof(user_id_text))) {
        return 0;
    }

    if (!read_input(
            "Enter 1 to enable, 0 to disable: ",
            status_text,
            sizeof(status_text))) {
        return 0;
    }

    char *end = NULL;
    errno = 0;

    long user_id = strtol(
        user_id_text,
        &end,
        10
    );

    if (errno != 0 ||
        end == user_id_text ||
        *end != '\0' ||
        user_id <= 0 ||
        user_id > 2147483647L) {
        printf("Invalid user ID.\n");
        return 0;
    }

    if (strcmp(status_text, "0") != 0 &&
        strcmp(status_text, "1") != 0) {
        printf("Status must be 0 or 1.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return 0;
    }

    cJSON_AddNumberToObject(
        data,
        "user_id",
        (double)user_id
    );

    cJSON_AddBoolToObject(
        data,
        "enabled",
        strcmp(status_text, "1") == 0
    );

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "SET_USER_STATUS",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    return parse_response(
        response_text,
        NULL,
        0,
        NULL,
        0
    );
}


static int perform_renew_book(
    int socket_fd,
    unsigned int *request_number
)
{
    char input[32];
    char request_id[32];
    char response_text[BUFFER_SIZE];

    if (!read_input(
            "Borrow record ID: ",
            input,
            sizeof(input))) {
        return 0;
    }

    char *end = NULL;
    errno = 0;
    long record_id = strtol(input, &end, 10);

    if (errno != 0 ||
        end == input ||
        *end != '\0' ||
        record_id <= 0 ||
        record_id > 2147483647L) {
        printf("Invalid record ID.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();

    if (data == NULL) {
        return 0;
    }

    cJSON_AddNumberToObject(
        data,
        "record_id",
        (double)record_id
    );

    create_request_id(
        request_id,
        sizeof(request_id),
        request_number
    );

    char *request = create_request(
        "RENEW_BOOK",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    return parse_response(
        response_text,
        NULL,
        0,
        NULL,
        0
    );
}

static int perform_update_book(
    int socket_fd,
    unsigned int *request_number
)
{
    char id_text[32];
    char title[128];
    char author[128];
    char publisher[128];
    char category[64];
    char location[64];
    char total_text[32];
    char request_id[32];
    char response_text[BUFFER_SIZE];

    if (!read_input("Book ID: ", id_text, sizeof(id_text)) ||
        !read_input("New title (blank keeps current): ", title, sizeof(title)) ||
        !read_input("New author (blank keeps current): ", author, sizeof(author)) ||
        !read_input("New publisher (blank keeps current): ", publisher,
            sizeof(publisher)) ||
        !read_input("New category (blank keeps current): ", category,
            sizeof(category)) ||
        !read_input("New location (blank keeps current): ", location,
            sizeof(location)) ||
        !read_input("New total count (blank keeps current): ", total_text,
            sizeof(total_text))) {
        return 0;
    }

    char *end = NULL;
    errno = 0;
    long book_id = strtol(id_text, &end, 10);
    if (errno != 0 || end == id_text || *end != '\0' ||
        book_id <= 0 || book_id > 2147483647L) {
        printf("Invalid book ID.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return 0;
    }
    cJSON_AddNumberToObject(data, "book_id", (double)book_id);

    int changed = 0;
    if (title[0] != '\0') {
        cJSON_AddStringToObject(data, "title", title);
        changed = 1;
    }
    if (author[0] != '\0') {
        cJSON_AddStringToObject(data, "author", author);
        changed = 1;
    }
    if (publisher[0] != '\0') {
        cJSON_AddStringToObject(data, "publisher", publisher);
        changed = 1;
    }
    if (category[0] != '\0') {
        cJSON_AddStringToObject(data, "category", category);
        changed = 1;
    }
    if (location[0] != '\0') {
        cJSON_AddStringToObject(data, "location", location);
        changed = 1;
    }

    if (total_text[0] != '\0') {
        end = NULL;
        errno = 0;
        long total = strtol(total_text, &end, 10);
        if (errno != 0 || end == total_text || *end != '\0' ||
            total <= 0 || total > 100000) {
            printf("Invalid total count.\n");
            cJSON_Delete(data);
            return 0;
        }
        cJSON_AddNumberToObject(data, "total_count", (double)total);
        changed = 1;
    }

    if (!changed) {
        printf("No changes entered.\n");
        cJSON_Delete(data);
        return 0;
    }

    create_request_id(request_id, sizeof(request_id), request_number);
    char *request = create_request("UPDATE_BOOK", request_id, data);
    if (!exchange_json(socket_fd, request, response_text,
            sizeof(response_text))) {
        return 0;
    }
    return parse_response(response_text, NULL, 0, NULL, 0);
}

static int perform_set_book_status(
    int socket_fd,
    unsigned int *request_number
)
{
    char id_text[32];
    char status_text[16];
    char request_id[32];
    char response_text[BUFFER_SIZE];

    if (!read_input("Book ID: ", id_text, sizeof(id_text)) ||
        !read_input("Enter 1 for NORMAL, 0 for DISABLED: ", status_text,
            sizeof(status_text))) {
        return 0;
    }

    char *end = NULL;
    errno = 0;
    long book_id = strtol(id_text, &end, 10);
    if (errno != 0 || end == id_text || *end != '\0' ||
        book_id <= 0 || book_id > 2147483647L) {
        printf("Invalid book ID.\n");
        return 0;
    }
    if (strcmp(status_text, "0") != 0 && strcmp(status_text, "1") != 0) {
        printf("Status must be 0 or 1.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return 0;
    }
    cJSON_AddNumberToObject(data, "book_id", (double)book_id);
    cJSON_AddStringToObject(data, "status",
        strcmp(status_text, "1") == 0 ? "NORMAL" : "DISABLED");
    create_request_id(request_id, sizeof(request_id), request_number);
    char *request = create_request("SET_BOOK_STATUS", request_id, data);
    if (!exchange_json(socket_fd, request, response_text,
            sizeof(response_text))) {
        return 0;
    }
    return parse_response(response_text, NULL, 0, NULL, 0);
}

static int perform_list_logs(
    int socket_fd,
    unsigned int *request_number
)
{
    char request_id[32];
    char response_text[BUFFER_SIZE];
    create_request_id(request_id, sizeof(request_id), request_number);
    char *request = create_request("LIST_LOGS", request_id, NULL);

    if (!exchange_json(socket_fd, request, response_text,
            sizeof(response_text))) {
        return 0;
    }

    cJSON *response = cJSON_Parse(response_text);
    cJSON *success = cJSON_IsObject(response)
        ? cJSON_GetObjectItemCaseSensitive(response, "success") : NULL;
    cJSON *data = cJSON_IsObject(response)
        ? cJSON_GetObjectItemCaseSensitive(response, "data") : NULL;
    cJSON *logs = cJSON_IsObject(data)
        ? cJSON_GetObjectItemCaseSensitive(data, "logs") : NULL;

    if (!cJSON_IsTrue(success) || !cJSON_IsArray(logs)) {
        cJSON *message = cJSON_IsObject(response)
            ? cJSON_GetObjectItemCaseSensitive(response, "message") : NULL;
        printf("Operation failed: %s\n",
            cJSON_IsString(message) ? message->valuestring : "Invalid response");
        cJSON_Delete(response);
        return 0;
    }

    printf("\n========== Operation Logs ==========\n");
    cJSON *log = NULL;
    cJSON_ArrayForEach(log, logs) {
        cJSON *ok = cJSON_GetObjectItemCaseSensitive(log, "success");
        printf("[%d] %s | %s (%s) | %s | %s | %s\n",
            get_json_int(log, "log_id"),
            get_json_string(log, "time"),
            get_json_string(log, "username"),
            get_json_string(log, "role"),
            get_json_string(log, "action"),
            cJSON_IsTrue(ok) ? "SUCCESS" : "FAILED",
            get_json_string(log, "message"));
    }
    printf("====================================\n");
    cJSON_Delete(response);
    return 1;
}

static int perform_change_credentials(
    int socket_fd,
    unsigned int *request_number
)
{
    char current_password[129];
    char new_username[64];
    char new_password[129];
    char request_id[32];
    char response_text[BUFFER_SIZE];

    printf("Leave a new value blank to keep it unchanged.\n");
    if (!read_input(
            "Current password: ",
            current_password,
            sizeof(current_password)) ||
        !read_input(
            "New username: ",
            new_username,
            sizeof(new_username)) ||
        !read_input(
            "New password: ",
            new_password,
            sizeof(new_password))) {
        return 0;
    }

    if (current_password[0] == '\0') {
        printf("Current password is required.\n");
        return 0;
    }
    if (new_username[0] == '\0' && new_password[0] == '\0') {
        printf("Enter a new username or password.\n");
        return 0;
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return 0;
    }

    cJSON_AddStringToObject(
        data,
        "current_password",
        current_password
    );
    if (new_username[0] != '\0') {
        cJSON_AddStringToObject(
            data,
            "new_username",
            new_username
        );
    }
    if (new_password[0] != '\0') {
        cJSON_AddStringToObject(
            data,
            "new_password",
            new_password
        );
    }

    create_request_id(request_id, sizeof(request_id), request_number);
    char *request = create_request(
        "CHANGE_CREDENTIALS",
        request_id,
        data
    );

    if (!exchange_json(
            socket_fd,
            request,
            response_text,
            sizeof(response_text))) {
        return 0;
    }

    return parse_response(
        response_text,
        NULL,
        0,
        NULL,
        0
    );
}

static int get_server_port(void)
{
    const char *value = getenv("LIBRARY_PORT");

    if (value == NULL || value[0] == '\0') {
        return DEFAULT_SERVER_PORT;
    }

    char *end = NULL;
    errno = 0;
    long port = strtol(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0' ||
        port < 1 || port > 65535) {
        fprintf(stderr, "Invalid LIBRARY_PORT.\n");
        return -1;
    }

    return (int)port;
}

int run_client(void)
{
    int client_fd;
    int server_port = get_server_port();
    const char *server_ip = getenv("LIBRARY_HOST");
    struct sockaddr_in server_addr;

    if (server_port < 0) {
        return EXIT_FAILURE;
    }

    if (server_ip == NULL || server_ip[0] == '\0') {
        server_ip = DEFAULT_SERVER_IP;
    }

    client_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (client_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );

    server_addr.sin_family = AF_INET;
    server_addr.sin_port =
        htons((uint16_t)server_port);

    if (inet_pton(
            AF_INET,
            server_ip,
            &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP.\n");
        close(client_fd);
        return EXIT_FAILURE;
    }

    printf(
        "Connecting to %s:%d...\n",
        server_ip,
        server_port
    );

    if (connect(
            client_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) == -1) {
        perror("connect");
        close(client_fd);
        return EXIT_FAILURE;
    }

    printf("Connected to library server.\n");

    unsigned int request_number = 1;
    int logged_in = 0;

    char current_name[NAME_SIZE] = "";
    char current_role[ROLE_SIZE] = "";
    char choice[16];

    while (1) {
        if (!logged_in) {
            printf("\n===== Library System =====\n");
            printf("1. Login\n");
            printf("2. Ping server\n");
            printf("0. Exit\n");

            if (!read_input(
                    "Select: ",
                    choice,
                    sizeof(choice))) {
                break;
            }

            if (strcmp(choice, "1") == 0) {
                logged_in = perform_login(
                    client_fd,
                    &request_number,
                    current_name,
                    sizeof(current_name),
                    current_role,
                    sizeof(current_role)
                );
            } else if (strcmp(choice, "2") == 0) {
                perform_ping(
                    client_fd,
                    &request_number
                );
            } else if (strcmp(choice, "0") == 0) {
                break;
            } else {
                printf("Invalid selection.\n");
            }
        } else {
            printf("\n===== Main Menu =====\n");
            printf(
                "Current user: %s (%s)\n",
                current_name,
                current_role
            );
            printf("1. View current user\n");
            printf("2. View all books\n");
	    printf("3. Search books\n");
	    printf("4. Borrow book\n");
	    printf("5. My borrow records\n");
	    printf("6. Return book\n");
	    printf("7. Renew book\n");
	    printf("8. Ping server\n");
            printf("9. Logout\n");

	    if (strcmp(current_role, "ADMIN") == 0) {
	        printf("10. Add book (Administrator)\n");
		printf("11. Add reader (Administrator)\n");
		printf("12. View all borrow records (Administrator)\n");
		printf("13. View users (Administrator)\n");
		printf("14. Enable/disable user (Administrator)\n");
		printf("15. Update book (Administrator)\n");
		printf("16. Enable/disable book (Administrator)\n");
		printf("17. View operation logs (Administrator)\n");
	    } else if (strcmp(current_role, "READER") == 0) {
		printf("10. Change username/password\n");
	    }

            printf("0. Exit\n");

            if (!read_input(
                    "Select: ",
                    choice,
                    sizeof(choice))) {
                break;
            }

            if (strcmp(choice, "1") == 0) {
                perform_simple_action(
                    client_fd,
                    "WHO_AM_I",
                    &request_number
                );
            } else if (strcmp(choice, "2") == 0) {
  		  perform_list_books(
        	      client_fd,
        	      &request_number
    		);
	    } else if (strcmp(choice, "3") == 0) {
    		  perform_search_books(
        	      client_fd,
        	      &request_number
    		);
	    } else if (strcmp(choice, "4") == 0) {
    		  perform_borrow_book(
        	      client_fd,
                      &request_number
    		);
	    }else if (strcmp(choice, "5") == 0) {
 		   perform_my_borrows(
        		client_fd,
        		&request_number
    		);
	    } else if (strcmp(choice, "6") == 0) {
    		   perform_return_book(
        	       client_fd,
        	       &request_number
    		);
	    } else if (strcmp(choice, "7") == 0) {
		perform_renew_book(
		    client_fd,
		    &request_number
		);
	    } else if (strcmp(choice, "8") == 0) {
		perform_ping(
                    client_fd,
                    &request_number
		);
	    } else if (strcmp(choice, "9") == 0) {
                if (perform_simple_action(
                        client_fd,
                        "LOGOUT",
                        &request_number)) {
                    logged_in = 0;
                    current_name[0] = '\0';
                    current_role[0] = '\0';
                }
	   } else if (strcmp(choice, "10") == 0) {
		if (strcmp(current_role, "ADMIN") == 0) {
		    perform_add_book(
			client_fd,
			&request_number
		    );
		} else if (strcmp(current_role, "READER") == 0) {
		    perform_change_credentials(
			client_fd,
			&request_number
		    );
		} else {
		    printf("Invalid selection.\n");
		}
	    }else if (
    		strcmp(choice, "11") == 0 &&
    		strcmp(current_role, "ADMIN") == 0
	    ) {
    		perform_add_user(
        	   client_fd,
        	   &request_number
    		);
	   }else if (
    		strcmp(choice, "12") == 0 &&
    		strcmp(current_role, "ADMIN") == 0
	    ) {
    		perform_all_borrows(
        	   client_fd,
        	   &request_number
    	     	);
	   }else if (
 		 strcmp(choice, "13") == 0 &&
    		 strcmp(current_role, "ADMIN") == 0
	    ) {
    		perform_list_users(
        	client_fd,
        	&request_number
    		);
	   } else if (
    		strcmp(choice, "14") == 0 &&
    		strcmp(current_role, "ADMIN") == 0
	    ) {
    		perform_set_user_status(
        	client_fd,
        	&request_number
    		);
	   } else if (
		strcmp(choice, "15") == 0 &&
		strcmp(current_role, "ADMIN") == 0
	    ) {
		perform_update_book(
		    client_fd,
		    &request_number
		);
	   } else if (
		strcmp(choice, "16") == 0 &&
		strcmp(current_role, "ADMIN") == 0
	    ) {
		perform_set_book_status(
		    client_fd,
		    &request_number
		);
	   } else if (
		strcmp(choice, "17") == 0 &&
		strcmp(current_role, "ADMIN") == 0
	    ) {
		perform_list_logs(
		    client_fd,
		    &request_number
		);
	   } else if (strcmp(choice, "0") == 0) {
                break;
            } else {
                printf("Invalid selection.\n");
            }
        }
    }

    close(client_fd);
    printf("Disconnected from server.\n");

    return EXIT_SUCCESS;
}
