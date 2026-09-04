#include "app.h"

#include <time.h>
#include <arpa/inet.h>
#include <cjson/cJSON.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_SERVER_PORT 18080
#define USERNAME_SIZE 64
#define NAME_SIZE 64
#define ROLE_SIZE 16
#define USERS_FILE "data/users.json"
#define BOOKS_FILE "data/books.json"
#define BORROW_RECORDS_FILE "data/borrow_records.json"
#define OPERATION_LOGS_FILE "data/operation_logs.json"
#define MAX_BORROW_COUNT 5
#define BORROW_DAYS 30
#define MAX_RENEW_COUNT 1
#define BUFFER_SIZE 65536
/*
 * 修改图书库存和借阅记录时必须持有这个锁，
 * 防止多个客户端同时修改数据。
 */
static pthread_mutex_t data_mutex =
    PTHREAD_MUTEX_INITIALIZER;

/* 每个客户端连接独立拥有一个会话 */
typedef struct {
    int logged_in;
    int user_id;
    char username[USERNAME_SIZE];
    char name[NAME_SIZE];
    char role[ROLE_SIZE];
} ClientSession;

static int parse_time_string(
    const char *text,
    time_t *result
);

/* 确保把指定长度的数据全部发送出去 */
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

/* 每条 JSON 消息以换行符结束 */
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

/*
 * 从 TCP 连接中读取一整行。
 *
 * 返回值：
 *   > 0：读取到的字符数量
 *     0：客户端断开连接
 *    -1：发生网络错误
 *    -2：消息过长
 */
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
            /*
             * 缓冲区已经满了，但继续读取，
             * 直到当前消息的换行符，避免破坏下一条消息。
             */
            too_long = 1;
        }
    }

    buffer[position] = '\0';

    if (too_long) {
        return -2;
    }

    return (ssize_t)position;
}

/* 读取整个文本文件，返回的字符串需要 free() */
static char *read_text_file(const char *file_path)
{
    FILE *file = fopen(file_path, "rb");

    if (file == NULL) {
        perror(file_path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long file_size = ftell(file);

    if (file_size < 0) {
        fclose(file);
        return NULL;
    }

    rewind(file);

    char *text = malloc((size_t)file_size + 1);

    if (text == NULL) {
        fclose(file);
        return NULL;
    }

    size_t read_size = fread(
        text,
        1,
        (size_t)file_size,
        file
    );

    fclose(file);

    if (read_size != (size_t)file_size) {
        free(text);
        return NULL;
    }

    text[read_size] = '\0';
    return text;
}

/*
 * 创建统一格式的 JSON 响应。
 *
 * 返回的字符串必须使用 cJSON_free() 释放。
 */
static char *create_response(
    int success,
    const char *request_id,
    const char *message,
    cJSON *data
)
{
    cJSON *response = cJSON_CreateObject();

    if (response == NULL) {
        cJSON_Delete(data);
        return NULL;
    }

    cJSON_AddBoolToObject(
        response,
        "success",
        success
    );

    if (request_id != NULL) {
        cJSON_AddStringToObject(
            response,
            "request_id",
            request_id
        );
    } else {
        cJSON_AddNullToObject(
            response,
            "request_id"
        );
    }

    cJSON_AddStringToObject(
        response,
        "message",
        message
    );

    if (data != NULL) {
        /* data 的所有权转交给 response */
        cJSON_AddItemToObject(
            response,
            "data",
            data
        );
    } else {
        cJSON_AddObjectToObject(
            response,
            "data"
        );
    }

    char *response_text =
        cJSON_PrintUnformatted(response);

    cJSON_Delete(response);
    return response_text;
}

static char *handle_ping(
    cJSON *request,
    const char *request_id
)
{
    cJSON *response_data = cJSON_CreateObject();

    if (response_data == NULL) {
        return NULL;
    }

    cJSON *request_data =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "data"
        );

    cJSON *request_message = NULL;

    if (cJSON_IsObject(request_data)) {
        request_message =
            cJSON_GetObjectItemCaseSensitive(
                request_data,
                "message"
            );
    }

    if (cJSON_IsString(request_message)) {
        cJSON_AddStringToObject(
            response_data,
            "echo",
            request_message->valuestring
        );
    } else {
        cJSON_AddStringToObject(
            response_data,
            "echo",
            ""
        );
    }

    return create_response(
        1,
        request_id,
        "pong",
        response_data
    );
}

static char *handle_login(
    cJSON *request,
    const char *request_id,
    ClientSession *session
)
{
    if (session->logged_in) {
        return create_response(
            0,
            request_id,
            "Already logged in",
            NULL
        );
    }

    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "data"
        );

    cJSON *username = NULL;
    cJSON *password = NULL;

    if (cJSON_IsObject(data)) {
        username =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "username"
            );

        password =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "password"
            );
    }

    if (!cJSON_IsString(username) ||
        !cJSON_IsString(password)) {
        return create_response(
            0,
            request_id,
            "Username and password are required",
            NULL
        );
    }

    char *file_text =
        read_text_file(USERS_FILE);

    if (file_text == NULL) {
        return create_response(
            0,
            request_id,
            "Cannot read users.json",
            NULL
        );
    }

    cJSON *users = cJSON_Parse(file_text);
    free(file_text);

    if (!cJSON_IsArray(users)) {
        cJSON_Delete(users);

        return create_response(
            0,
            request_id,
            "Invalid users.json",
            NULL
        );
    }

    char *response = NULL;
    cJSON *user = NULL;

    cJSON_ArrayForEach(user, users) {
        cJSON *stored_username =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "username"
            );

        cJSON *stored_password =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "password"
            );

        if (!cJSON_IsString(stored_username) ||
            !cJSON_IsString(stored_password)) {
            continue;
        }

        if (strcmp(
                username->valuestring,
                stored_username->valuestring) != 0) {
            continue;
        }

        if (strcmp(
                password->valuestring,
                stored_password->valuestring) != 0) {
            continue;
        }

        cJSON *enabled =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "enabled"
            );

        if (!cJSON_IsTrue(enabled)) {
            response = create_response(
                0,
                request_id,
                "Account is disabled",
                NULL
            );
            break;
        }

        cJSON *id =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "id"
            );

        cJSON *name =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "name"
            );

        cJSON *role =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "role"
            );

        if (!cJSON_IsNumber(id) ||
            !cJSON_IsString(name) ||
            !cJSON_IsString(role)) {
            response = create_response(
                0,
                request_id,
                "Invalid user record",
                NULL
            );
            break;
        }

        if (strcmp(role->valuestring, "ADMIN") != 0 &&
            strcmp(role->valuestring, "READER") != 0) {
            response = create_response(
                0,
                request_id,
                "Invalid user role",
                NULL
            );
            break;
        }

        session->logged_in = 1;
        session->user_id = id->valueint;

        snprintf(
            session->username,
            sizeof(session->username),
            "%s",
            stored_username->valuestring
        );

        snprintf(
            session->name,
            sizeof(session->name),
            "%s",
            name->valuestring
        );

        snprintf(
            session->role,
            sizeof(session->role),
            "%s",
            role->valuestring
        );

        cJSON *response_data =
            cJSON_CreateObject();

        if (response_data == NULL) {
            response = NULL;
            break;
        }

        cJSON_AddNumberToObject(
            response_data,
            "user_id",
            session->user_id
        );

        cJSON_AddStringToObject(
            response_data,
            "username",
            session->username
        );

        cJSON_AddStringToObject(
            response_data,
            "name",
            session->name
        );

        cJSON_AddStringToObject(
            response_data,
            "role",
            session->role
        );

        response = create_response(
            1,
            request_id,
            "Login successful",
            response_data
        );

        break;
    }

    if (response == NULL && !session->logged_in) {
        response = create_response(
            0,
            request_id,
            "Invalid username or password",
            NULL
        );
    }

    cJSON_Delete(users);
    return response;
}

static char *handle_logout(
    const char *request_id,
    ClientSession *session
)
{
    if (!session->logged_in) {
        return create_response(
            0,
            request_id,
            "Not logged in",
            NULL
        );
    }

    memset(session, 0, sizeof(*session));

    return create_response(
        1,
        request_id,
        "Logout successful",
        NULL
    );
}

static char *handle_who_am_i(
    const char *request_id,
    const ClientSession *session
)
{
    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(
        response_data,
        "user_id",
        session->user_id
    );

    cJSON_AddStringToObject(
        response_data,
        "username",
        session->username
    );

    cJSON_AddStringToObject(
        response_data,
        "name",
        session->name
    );

    cJSON_AddStringToObject(
        response_data,
        "role",
        session->role
    );

    return create_response(
        1,
        request_id,
        "Current user",
        response_data
    );
}

static char *handle_list_books(
    const char *request_id
)
{
    char *file_text = read_text_file(BOOKS_FILE);

    if (file_text == NULL) {
        return create_response(
            0,
            request_id,
            "Cannot read books.json",
            NULL
        );
    }

    cJSON *books = cJSON_Parse(file_text);
    free(file_text);

    if (!cJSON_IsArray(books)) {
        cJSON_Delete(books);

        return create_response(
            0,
            request_id,
            "Invalid books.json",
            NULL
        );
    }

    cJSON *response_data = cJSON_CreateObject();

    if (response_data == NULL) {
        cJSON_Delete(books);
        return NULL;
    }

    cJSON_AddNumberToObject(
        response_data,
        "count",
        cJSON_GetArraySize(books)
    );

    /*
     * books 的所有权转交给 response_data，
     * 后面不需要单独 cJSON_Delete(books)。
     */
    cJSON_AddItemToObject(
        response_data,
        "books",
        books
    );

    return create_response(
        1,
        request_id,
        "Book list retrieved",
        response_data
    );
}

/* ASCII 字母转小写；中文 UTF-8 字节保持不变 */
static unsigned char lower_ascii(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (unsigned char)(ch + ('a' - 'A'));
    }

    return ch;
}

/*
 * 忽略英文字母大小写的子串搜索。
 * 中文搜索仍然可以正常进行，但要求文字完全一致。
 */
static int contains_ignore_case(
    const char *text,
    const char *keyword
)
{
    if (text == NULL || keyword == NULL) {
        return 0;
    }

    if (keyword[0] == '\0') {
        return 1;
    }

    for (size_t i = 0; text[i] != '\0'; i++) {
        size_t text_position = i;
        size_t keyword_position = 0;

        while (
            text[text_position] != '\0' &&
            keyword[keyword_position] != '\0' &&
            lower_ascii((unsigned char)text[text_position]) ==
                lower_ascii((unsigned char)keyword[keyword_position])
        ) {
            text_position++;
            keyword_position++;
        }

        if (keyword[keyword_position] == '\0') {
            return 1;
        }
    }

    return 0;
}

static int book_matches_keyword(
    const cJSON *book,
    const char *keyword
)
{
    const char *field_names[] = {
        "title",
        "author",
        "isbn",
        "category",
        "publisher"
    };

    size_t field_count =
        sizeof(field_names) / sizeof(field_names[0]);

    for (size_t i = 0; i < field_count; i++) {
        cJSON *field =
            cJSON_GetObjectItemCaseSensitive(
                book,
                field_names[i]
            );

        if (cJSON_IsString(field) &&
            contains_ignore_case(
                field->valuestring,
                keyword)) {
            return 1;
        }
    }

    return 0;
}

static char *handle_search_books(
    cJSON *request,
    const char *request_id
)
{
    cJSON *request_data =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "data"
        );

    cJSON *keyword_item = NULL;

    if (cJSON_IsObject(request_data)) {
        keyword_item =
            cJSON_GetObjectItemCaseSensitive(
                request_data,
                "keyword"
            );
    }

    if (!cJSON_IsString(keyword_item) ||
        keyword_item->valuestring[0] == '\0') {
        return create_response(
            0,
            request_id,
            "Search keyword is required",
            NULL
        );
    }

    char *file_text =
        read_text_file(BOOKS_FILE);

    if (file_text == NULL) {
        return create_response(
            0,
            request_id,
            "Cannot read books.json",
            NULL
        );
    }

    cJSON *books = cJSON_Parse(file_text);
    free(file_text);

    if (!cJSON_IsArray(books)) {
        cJSON_Delete(books);

        return create_response(
            0,
            request_id,
            "Invalid books.json",
            NULL
        );
    }

    cJSON *matched_books =
        cJSON_CreateArray();

    if (matched_books == NULL) {
        cJSON_Delete(books);
        return NULL;
    }

    cJSON *book = NULL;

    cJSON_ArrayForEach(book, books) {
        if (book_matches_keyword(
                book,
                keyword_item->valuestring)) {
            cJSON *book_copy =
                cJSON_Duplicate(book, 1);

            if (book_copy != NULL) {
                cJSON_AddItemToArray(
                    matched_books,
                    book_copy
                );
            }
        }
    }

    cJSON_Delete(books);

    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data == NULL) {
        cJSON_Delete(matched_books);
        return NULL;
    }

    cJSON_AddNumberToObject(
        response_data,
        "count",
        cJSON_GetArraySize(matched_books)
    );

    cJSON_AddStringToObject(
        response_data,
        "keyword",
        keyword_item->valuestring
    );

    cJSON_AddItemToObject(
        response_data,
        "books",
        matched_books
    );

    return create_response(
        1,
        request_id,
        "Search completed",
        response_data
    );
}


/*
 * 先写入临时文件，再用 rename() 替换原文件，
 * 避免程序写到一半时破坏原 JSON。
 */
static int write_json_file_atomic(
    const char *file_path,
    const cJSON *json
)
{
    char temporary_path[256];

    int path_length = snprintf(
        temporary_path,
        sizeof(temporary_path),
        "%s.tmp",
        file_path
    );

    if (path_length < 0 ||
        (size_t)path_length >= sizeof(temporary_path)) {
        return -1;
    }

    char *text = cJSON_Print(json);

    if (text == NULL) {
        return -1;
    }

    FILE *file = fopen(temporary_path, "wb");

    if (file == NULL) {
        perror(temporary_path);
        cJSON_free(text);
        return -1;
    }

    size_t text_length = strlen(text);

    size_t written = fwrite(
        text,
        1,
        text_length,
        file
    );

    int failed = 0;

    if (written != text_length) {
        failed = 1;
    }

    if (fflush(file) != 0) {
        failed = 1;
    }

    if (fclose(file) != 0) {
        failed = 1;
    }

    cJSON_free(text);

    if (failed) {
        remove(temporary_path);
        return -1;
    }

    if (rename(
            temporary_path,
            file_path) != 0) {
        perror("rename");
        remove(temporary_path);
        return -1;
    }

    return 0;
}

static cJSON *read_json_array_file(
    const char *file_path
)
{
    char *text = read_text_file(file_path);

    if (text == NULL) {
        return NULL;
    }

    cJSON *array = cJSON_Parse(text);
    free(text);

    if (!cJSON_IsArray(array)) {
        cJSON_Delete(array);
        return NULL;
    }

    return array;
}

static void format_time_string(
    time_t value,
    char *buffer,
    size_t capacity
)
{
    struct tm time_info;

    localtime_r(&value, &time_info);

    strftime(
        buffer,
        capacity,
        "%Y-%m-%d %H:%M:%S",
        &time_info
    );
}

static int record_is_active(const cJSON *record)
{
    cJSON *status = cJSON_GetObjectItemCaseSensitive(
        record,
        "status"
    );

    return cJSON_IsString(status) &&
        (strcmp(status->valuestring, "BORROWED") == 0 ||
         strcmp(status->valuestring, "OVERDUE") == 0);
}

static int record_is_overdue(const cJSON *record)
{
    if (!record_is_active(record)) {
        return 0;
    }

    cJSON *due_time = cJSON_GetObjectItemCaseSensitive(
        record,
        "due_time"
    );
    time_t parsed_due_time;

    return cJSON_IsString(due_time) &&
        parse_time_string(due_time->valuestring, &parsed_due_time) &&
        time(NULL) > parsed_due_time;
}

static char *handle_borrow_book(
    cJSON *request,
    const char *request_id,
    const ClientSession *session
)
{
    cJSON *request_data =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "data"
        );

    cJSON *book_id_item = NULL;

    if (cJSON_IsObject(request_data)) {
        book_id_item =
            cJSON_GetObjectItemCaseSensitive(
                request_data,
                "book_id"
            );
    }

    if (!cJSON_IsNumber(book_id_item) ||
        book_id_item->valueint <= 0) {
        return create_response(
            0,
            request_id,
            "Valid book_id is required",
            NULL
        );
    }

    int requested_book_id =
        book_id_item->valueint;

    char *response = NULL;
    cJSON *books = NULL;
    cJSON *records = NULL;

    pthread_mutex_lock(&data_mutex);

    books = read_json_array_file(BOOKS_FILE);
    records = read_json_array_file(
        BORROW_RECORDS_FILE
    );

    if (books == NULL || records == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot read library data",
            NULL
        );

        goto cleanup;
    }

    cJSON *target_book = NULL;
    cJSON *book = NULL;

    cJSON_ArrayForEach(book, books) {
        cJSON *id =
            cJSON_GetObjectItemCaseSensitive(
                book,
                "id"
            );

        if (cJSON_IsNumber(id) &&
            id->valueint == requested_book_id) {
            target_book = book;
            break;
        }
    }

    if (target_book == NULL) {
        response = create_response(
            0,
            request_id,
            "Book not found",
            NULL
        );

        goto cleanup;
    }

    cJSON *title =
        cJSON_GetObjectItemCaseSensitive(
            target_book,
            "title"
        );

    cJSON *available_count =
        cJSON_GetObjectItemCaseSensitive(
            target_book,
            "available_count"
        );

    cJSON *book_status =
        cJSON_GetObjectItemCaseSensitive(
            target_book,
            "status"
        );

    if (!cJSON_IsString(title) ||
        !cJSON_IsNumber(available_count) ||
        !cJSON_IsString(book_status)) {
        response = create_response(
            0,
            request_id,
            "Invalid book record",
            NULL
        );

        goto cleanup;
    }

    if (strcmp(
            book_status->valuestring,
            "NORMAL") != 0) {
        response = create_response(
            0,
            request_id,
            "Book is not available for borrowing",
            NULL
        );

        goto cleanup;
    }

    if (available_count->valueint <= 0) {
        response = create_response(
            0,
            request_id,
            "No available copies",
            NULL
        );

        goto cleanup;
    }

    int active_borrow_count = 0;
    int next_record_id = 1;
    cJSON *record = NULL;

    cJSON_ArrayForEach(record, records) {
        cJSON *record_id =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "record_id"
            );

        if (cJSON_IsNumber(record_id) &&
            record_id->valueint >= next_record_id) {
            next_record_id =
                record_id->valueint + 1;
        }

        cJSON *record_user_id =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "user_id"
            );

        cJSON *record_book_id =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "book_id"
            );

        cJSON *record_status =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "status"
            );

        if (!cJSON_IsNumber(record_user_id) ||
            !cJSON_IsNumber(record_book_id) ||
            !cJSON_IsString(record_status)) {
            continue;
        }

        if (record_user_id->valueint !=
            session->user_id) {
            continue;
        }

        if (!record_is_active(record)) {
            continue;
        }

        if (record_is_overdue(record)) {
            response = create_response(
                0,
                request_id,
                "Return overdue books before borrowing again",
                NULL
            );
            goto cleanup;
        }

        active_borrow_count++;

        if (record_book_id->valueint ==
            requested_book_id) {
            response = create_response(
                0,
                request_id,
                "You have already borrowed this book",
                NULL
            );

            goto cleanup;
        }
    }

    if (active_borrow_count >= MAX_BORROW_COUNT) {
        response = create_response(
            0,
            request_id,
            "Borrowing limit reached",
            NULL
        );

        goto cleanup;
    }

    time_t borrow_time = time(NULL);
    time_t due_time =
        borrow_time +
        (time_t)BORROW_DAYS * 24 * 60 * 60;

    char borrow_time_text[32];
    char due_time_text[32];

    format_time_string(
        borrow_time,
        borrow_time_text,
        sizeof(borrow_time_text)
    );

    format_time_string(
        due_time,
        due_time_text,
        sizeof(due_time_text)
    );

    cJSON *new_record = cJSON_CreateObject();

    if (new_record == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot create borrow record",
            NULL
        );

        goto cleanup;
    }

    cJSON_AddNumberToObject(
        new_record,
        "record_id",
        next_record_id
    );

    cJSON_AddNumberToObject(
        new_record,
        "user_id",
        session->user_id
    );

    cJSON_AddNumberToObject(
        new_record,
        "book_id",
        requested_book_id
    );

    cJSON_AddStringToObject(
        new_record,
        "borrow_time",
        borrow_time_text
    );

    cJSON_AddStringToObject(
        new_record,
        "due_time",
        due_time_text
    );

    cJSON_AddNullToObject(
        new_record,
        "return_time"
    );

    cJSON_AddNumberToObject(
        new_record,
        "renew_count",
        0
    );

    cJSON_AddStringToObject(
        new_record,
        "status",
        "BORROWED"
    );

    cJSON_AddItemToArray(
        records,
        new_record
    );

    int old_available_count =
        available_count->valueint;

    cJSON_SetNumberValue(
        available_count,
        old_available_count - 1
    );

    /*
     * 先保存图书库存，再保存借阅记录。
     * 如果记录保存失败，尝试恢复库存。
     */
    if (write_json_file_atomic(
            BOOKS_FILE,
            books) != 0) {
        response = create_response(
            0,
            request_id,
            "Cannot save book inventory",
            NULL
        );

        goto cleanup;
    }

    if (write_json_file_atomic(
            BORROW_RECORDS_FILE,
            records) != 0) {
        cJSON_SetNumberValue(
            available_count,
            old_available_count
        );

        write_json_file_atomic(
            BOOKS_FILE,
            books
        );

        response = create_response(
            0,
            request_id,
            "Cannot save borrow record",
            NULL
        );

        goto cleanup;
    }

    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data == NULL) {
        response = NULL;
        goto cleanup;
    }

    cJSON_AddNumberToObject(
        response_data,
        "record_id",
        next_record_id
    );

    cJSON_AddNumberToObject(
        response_data,
        "book_id",
        requested_book_id
    );

    cJSON_AddStringToObject(
        response_data,
        "title",
        title->valuestring
    );

    cJSON_AddStringToObject(
        response_data,
        "borrow_time",
        borrow_time_text
    );

    cJSON_AddStringToObject(
        response_data,
        "due_time",
        due_time_text
    );

    cJSON_AddNumberToObject(
        response_data,
        "available_count",
        old_available_count - 1
    );

    response = create_response(
        1,
        request_id,
        "Book borrowed successfully",
        response_data
    );

cleanup:
    cJSON_Delete(books);
    cJSON_Delete(records);

    pthread_mutex_unlock(&data_mutex);

    return response;
}


static char *handle_my_borrows(
    const char *request_id,
    const ClientSession *session
)
{
    char *response = NULL;
    cJSON *books = NULL;
    cJSON *records = NULL;

    pthread_mutex_lock(&data_mutex);

    books = read_json_array_file(BOOKS_FILE);
    records = read_json_array_file(
        BORROW_RECORDS_FILE
    );

    if (books == NULL || records == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot read library data",
            NULL
        );

        goto cleanup;
    }

    cJSON *result_records =
        cJSON_CreateArray();

    if (result_records == NULL) {
        goto cleanup;
    }

    cJSON *record = NULL;

    cJSON_ArrayForEach(record, records) {
        cJSON *user_id =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "user_id"
            );

        if (!cJSON_IsNumber(user_id) ||
            user_id->valueint != session->user_id) {
            continue;
        }

        cJSON *record_copy =
            cJSON_Duplicate(record, 1);

        if (record_copy == NULL) {
            continue;
        }

        if (record_is_overdue(record)) {
            cJSON_ReplaceItemInObjectCaseSensitive(
                record_copy,
                "status",
                cJSON_CreateString("OVERDUE")
            );
        }

        cJSON *record_book_id =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "book_id"
            );

        const char *book_title = "Unknown book";

        if (cJSON_IsNumber(record_book_id)) {
            cJSON *book = NULL;

            cJSON_ArrayForEach(book, books) {
                cJSON *book_id =
                    cJSON_GetObjectItemCaseSensitive(
                        book,
                        "id"
                    );

                if (cJSON_IsNumber(book_id) &&
                    book_id->valueint ==
                        record_book_id->valueint) {
                    cJSON *title =
                        cJSON_GetObjectItemCaseSensitive(
                            book,
                            "title"
                        );

                    if (cJSON_IsString(title)) {
                        book_title =
                            title->valuestring;
                    }

                    break;
                }
            }
        }

        cJSON_AddStringToObject(
            record_copy,
            "title",
            book_title
        );

        cJSON_AddItemToArray(
            result_records,
            record_copy
        );
    }

    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data == NULL) {
        cJSON_Delete(result_records);
        goto cleanup;
    }

    cJSON_AddNumberToObject(
        response_data,
        "count",
        cJSON_GetArraySize(result_records)
    );

    cJSON_AddItemToObject(
        response_data,
        "records",
        result_records
    );

    response = create_response(
        1,
        request_id,
        "Borrow records retrieved",
        response_data
    );

cleanup:
    cJSON_Delete(books);
    cJSON_Delete(records);

    pthread_mutex_unlock(&data_mutex);

    if (response == NULL) {
        return create_response(
            0,
            request_id,
            "Cannot create response",
            NULL
        );
    }

    return response;
}

static char *handle_return_book(
    cJSON *request,
    const char *request_id,
    const ClientSession *session
)
{
    cJSON *request_data =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "data"
        );

    cJSON *record_id_item = NULL;

    if (cJSON_IsObject(request_data)) {
        record_id_item =
            cJSON_GetObjectItemCaseSensitive(
                request_data,
                "record_id"
            );
    }

    if (!cJSON_IsNumber(record_id_item) ||
        record_id_item->valueint <= 0) {
        return create_response(
            0,
            request_id,
            "Valid record_id is required",
            NULL
        );
    }

    int requested_record_id =
        record_id_item->valueint;

    char *response = NULL;
    cJSON *books = NULL;
    cJSON *records = NULL;

    pthread_mutex_lock(&data_mutex);

    books = read_json_array_file(BOOKS_FILE);
    records = read_json_array_file(
        BORROW_RECORDS_FILE
    );

    if (books == NULL || records == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot read library data",
            NULL
        );

        goto cleanup;
    }

    cJSON *target_record = NULL;
    cJSON *record = NULL;

    cJSON_ArrayForEach(record, records) {
        cJSON *record_id =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "record_id"
            );

        cJSON *user_id =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "user_id"
            );

        if (cJSON_IsNumber(record_id) &&
            cJSON_IsNumber(user_id) &&
            record_id->valueint ==
                requested_record_id &&
            user_id->valueint ==
                session->user_id) {
            target_record = record;
            break;
        }
    }

    if (target_record == NULL) {
        response = create_response(
            0,
            request_id,
            "Borrow record not found",
            NULL
        );

        goto cleanup;
    }

    cJSON *record_status =
        cJSON_GetObjectItemCaseSensitive(
            target_record,
            "status"
        );

    if (!cJSON_IsString(record_status) ||
        !record_is_active(target_record)) {
        response = create_response(
            0,
            request_id,
            "Book has already been returned",
            NULL
        );

        goto cleanup;
    }

    cJSON *record_book_id =
        cJSON_GetObjectItemCaseSensitive(
            target_record,
            "book_id"
        );

    if (!cJSON_IsNumber(record_book_id)) {
        response = create_response(
            0,
            request_id,
            "Invalid borrow record",
            NULL
        );

        goto cleanup;
    }

    cJSON *target_book = NULL;
    cJSON *book = NULL;

    cJSON_ArrayForEach(book, books) {
        cJSON *book_id =
            cJSON_GetObjectItemCaseSensitive(
                book,
                "id"
            );

        if (cJSON_IsNumber(book_id) &&
            book_id->valueint ==
                record_book_id->valueint) {
            target_book = book;
            break;
        }
    }

    if (target_book == NULL) {
        response = create_response(
            0,
            request_id,
            "Book information not found",
            NULL
        );

        goto cleanup;
    }

    cJSON *available_count =
        cJSON_GetObjectItemCaseSensitive(
            target_book,
            "available_count"
        );

    cJSON *total_count =
        cJSON_GetObjectItemCaseSensitive(
            target_book,
            "total_count"
        );

    cJSON *title =
        cJSON_GetObjectItemCaseSensitive(
            target_book,
            "title"
        );

    if (!cJSON_IsNumber(available_count) ||
        !cJSON_IsNumber(total_count) ||
        !cJSON_IsString(title)) {
        response = create_response(
            0,
            request_id,
            "Invalid book record",
            NULL
        );

        goto cleanup;
    }

    if (available_count->valueint >=
        total_count->valueint) {
        response = create_response(
            0,
            request_id,
            "Book inventory is inconsistent",
            NULL
        );

        goto cleanup;
    }

    int old_available_count =
        available_count->valueint;

    time_t return_time = time(NULL);
    char return_time_text[32];

    format_time_string(
        return_time,
        return_time_text,
        sizeof(return_time_text)
    );

    cJSON_SetNumberValue(
        available_count,
        old_available_count + 1
    );

    cJSON_DeleteItemFromObjectCaseSensitive(
        target_record,
        "return_time"
    );

    cJSON_AddStringToObject(
        target_record,
        "return_time",
        return_time_text
    );

    cJSON_DeleteItemFromObjectCaseSensitive(
        target_record,
        "status"
    );

    cJSON_AddStringToObject(
        target_record,
        "status",
        "RETURNED"
    );

    /*
     * 先保存库存，再保存借阅记录。
     * 如果记录保存失败，尝试把库存恢复。
     */
    if (write_json_file_atomic(
            BOOKS_FILE,
            books) != 0) {
        response = create_response(
            0,
            request_id,
            "Cannot save book inventory",
            NULL
        );

        goto cleanup;
    }

    if (write_json_file_atomic(
            BORROW_RECORDS_FILE,
            records) != 0) {
        cJSON_SetNumberValue(
            available_count,
            old_available_count
        );

        write_json_file_atomic(
            BOOKS_FILE,
            books
        );

        response = create_response(
            0,
            request_id,
            "Cannot save return record",
            NULL
        );

        goto cleanup;
    }

    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data != NULL) {
        cJSON_AddNumberToObject(
            response_data,
            "record_id",
            requested_record_id
        );

        cJSON_AddNumberToObject(
            response_data,
            "book_id",
            record_book_id->valueint
        );

        cJSON_AddStringToObject(
            response_data,
            "title",
            title->valuestring
        );

        cJSON_AddStringToObject(
            response_data,
            "return_time",
            return_time_text
        );

        cJSON_AddNumberToObject(
            response_data,
            "available_count",
            old_available_count + 1
        );
    }

    response = create_response(
        1,
        request_id,
        "Book returned successfully",
        response_data
    );

cleanup:
    cJSON_Delete(books);
    cJSON_Delete(records);

    pthread_mutex_unlock(&data_mutex);

    return response;
}

static int is_admin(
    const ClientSession *session
)
{
    return session->logged_in &&
           strcmp(session->role, "ADMIN") == 0;
}

static int is_nonempty_string(
    const cJSON *item
)
{
    return cJSON_IsString(item) &&
           item->valuestring[0] != '\0';
}

static char *handle_add_book(
    cJSON *request,
    const char *request_id,
    const ClientSession *session
)
{
    if (!is_admin(session)) {
        return create_response(
            0,
            request_id,
            "Administrator permission required",
            NULL
        );
    }

    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "data"
        );

    if (!cJSON_IsObject(data)) {
        return create_response(
            0,
            request_id,
            "Book data is required",
            NULL
        );
    }

    cJSON *isbn =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "isbn"
        );

    cJSON *title =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "title"
        );

    cJSON *author =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "author"
        );

    cJSON *publisher =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "publisher"
        );

    cJSON *category =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "category"
        );

    cJSON *location =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "location"
        );

    cJSON *total_count =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "total_count"
        );

    if (!is_nonempty_string(isbn) ||
        !is_nonempty_string(title) ||
        !is_nonempty_string(author) ||
        !is_nonempty_string(publisher) ||
        !is_nonempty_string(category) ||
        !is_nonempty_string(location) ||
        !cJSON_IsNumber(total_count) ||
        total_count->valueint <= 0) {
        return create_response(
            0,
            request_id,
            "Invalid book data",
            NULL
        );
    }

    char *response = NULL;
    cJSON *books = NULL;

    pthread_mutex_lock(&data_mutex);

    books = read_json_array_file(BOOKS_FILE);

    if (books == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot read books.json",
            NULL
        );

        goto cleanup;
    }

    int next_book_id = 1001;
    cJSON *book = NULL;

    cJSON_ArrayForEach(book, books) {
        cJSON *existing_id =
            cJSON_GetObjectItemCaseSensitive(
                book,
                "id"
            );

        cJSON *existing_isbn =
            cJSON_GetObjectItemCaseSensitive(
                book,
                "isbn"
            );

        if (cJSON_IsNumber(existing_id) &&
            existing_id->valueint >= next_book_id) {
            next_book_id =
                existing_id->valueint + 1;
        }

        if (cJSON_IsString(existing_isbn) &&
            strcmp(
                existing_isbn->valuestring,
                isbn->valuestring) == 0) {
            response = create_response(
                0,
                request_id,
                "ISBN already exists",
                NULL
            );

            goto cleanup;
        }
    }

    cJSON *new_book = cJSON_CreateObject();

    if (new_book == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot create book",
            NULL
        );

        goto cleanup;
    }

    cJSON_AddNumberToObject(
        new_book,
        "id",
        next_book_id
    );

    cJSON_AddStringToObject(
        new_book,
        "isbn",
        isbn->valuestring
    );

    cJSON_AddStringToObject(
        new_book,
        "title",
        title->valuestring
    );

    cJSON_AddStringToObject(
        new_book,
        "author",
        author->valuestring
    );

    cJSON_AddStringToObject(
        new_book,
        "publisher",
        publisher->valuestring
    );

    cJSON_AddStringToObject(
        new_book,
        "category",
        category->valuestring
    );

    cJSON_AddNumberToObject(
        new_book,
        "total_count",
        total_count->valueint
    );

    cJSON_AddNumberToObject(
        new_book,
        "available_count",
        total_count->valueint
    );

    cJSON_AddStringToObject(
        new_book,
        "location",
        location->valuestring
    );

    cJSON_AddStringToObject(
        new_book,
        "status",
        "NORMAL"
    );

    cJSON_AddItemToArray(
        books,
        new_book
    );

    if (write_json_file_atomic(
            BOOKS_FILE,
            books) != 0) {
        response = create_response(
            0,
            request_id,
            "Cannot save books.json",
            NULL
        );

        goto cleanup;
    }

    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data != NULL) {
        cJSON_AddNumberToObject(
            response_data,
            "book_id",
            next_book_id
        );

        cJSON_AddStringToObject(
            response_data,
            "isbn",
            isbn->valuestring
        );

        cJSON_AddStringToObject(
            response_data,
            "title",
            title->valuestring
        );

        cJSON_AddNumberToObject(
            response_data,
            "total_count",
            total_count->valueint
        );
    }

    response = create_response(
        1,
        request_id,
        "Book added successfully",
        response_data
    );

cleanup:
    cJSON_Delete(books);
    pthread_mutex_unlock(&data_mutex);

    return response;
}

static char *handle_add_user(
    cJSON *request,
    const char *request_id,
    const ClientSession *session
)
{
    if (!is_admin(session)) {
        return create_response(
            0,
            request_id,
            "Administrator permission required",
            NULL
        );
    }

    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "data"
        );

    if (!cJSON_IsObject(data)) {
        return create_response(
            0,
            request_id,
            "User data is required",
            NULL
        );
    }

    cJSON *username =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "username"
        );

    cJSON *password =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "password"
        );

    cJSON *name =
        cJSON_GetObjectItemCaseSensitive(
            data,
            "name"
        );

    if (!is_nonempty_string(username) ||
        !is_nonempty_string(password) ||
        !is_nonempty_string(name)) {
        return create_response(
            0,
            request_id,
            "Invalid user data",
            NULL
        );
    }

    if (strlen(username->valuestring) >= USERNAME_SIZE ||
        strlen(name->valuestring) >= NAME_SIZE ||
        strlen(password->valuestring) > 128) {
        return create_response(
            0,
            request_id,
            "User data is too long",
            NULL
        );
    }

    char *response = NULL;
    cJSON *users = NULL;

    pthread_mutex_lock(&data_mutex);

    users = read_json_array_file(USERS_FILE);

    if (users == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot read users.json",
            NULL
        );

        goto cleanup;
    }

    int next_user_id = 1;
    cJSON *user = NULL;

    cJSON_ArrayForEach(user, users) {
        cJSON *existing_id =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "id"
            );

        cJSON *existing_username =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "username"
            );

        if (cJSON_IsNumber(existing_id) &&
            existing_id->valueint >= next_user_id) {
            next_user_id =
                existing_id->valueint + 1;
        }

        if (cJSON_IsString(existing_username) &&
            strcmp(
                existing_username->valuestring,
                username->valuestring) == 0) {
            response = create_response(
                0,
                request_id,
                "Username already exists",
                NULL
            );

            goto cleanup;
        }
    }

    cJSON *new_user = cJSON_CreateObject();

    if (new_user == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot create user",
            NULL
        );

        goto cleanup;
    }

    cJSON_AddNumberToObject(
        new_user,
        "id",
        next_user_id
    );

    cJSON_AddStringToObject(
        new_user,
        "username",
        username->valuestring
    );

    /*
     * 当前阶段仍使用明文密码。
     * 完成功能后再升级为密码哈希。
     */
    cJSON_AddStringToObject(
        new_user,
        "password",
        password->valuestring
    );

    cJSON_AddStringToObject(
        new_user,
        "name",
        name->valuestring
    );

    /* 管理员只能通过此接口创建普通读者 */
    cJSON_AddStringToObject(
        new_user,
        "role",
        "READER"
    );

    cJSON_AddBoolToObject(
        new_user,
        "enabled",
        1
    );

    cJSON_AddItemToArray(
        users,
        new_user
    );

    if (write_json_file_atomic(
            USERS_FILE,
            users) != 0) {
        response = create_response(
            0,
            request_id,
            "Cannot save users.json",
            NULL
        );

        goto cleanup;
    }

    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data != NULL) {
        cJSON_AddNumberToObject(
            response_data,
            "user_id",
            next_user_id
        );

        cJSON_AddStringToObject(
            response_data,
            "username",
            username->valuestring
        );

        cJSON_AddStringToObject(
            response_data,
            "name",
            name->valuestring
        );

        cJSON_AddStringToObject(
            response_data,
            "role",
            "READER"
        );
    }

    response = create_response(
        1,
        request_id,
        "User added successfully",
        response_data
    );

cleanup:
    cJSON_Delete(users);
    pthread_mutex_unlock(&data_mutex);

    return response;
}

static char *handle_all_borrows(
    const char *request_id,
    const ClientSession *session
)
{
    if (!is_admin(session)) {
        return create_response(
            0,
            request_id,
            "Administrator permission required",
            NULL
        );
    }

    char *response = NULL;
    cJSON *users = NULL;
    cJSON *books = NULL;
    cJSON *records = NULL;

    pthread_mutex_lock(&data_mutex);

    users = read_json_array_file(USERS_FILE);
    books = read_json_array_file(BOOKS_FILE);
    records = read_json_array_file(
        BORROW_RECORDS_FILE
    );

    if (users == NULL ||
        books == NULL ||
        records == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot read library data",
            NULL
        );

        goto cleanup;
    }

    cJSON *result_records =
        cJSON_CreateArray();

    if (result_records == NULL) {
        goto cleanup;
    }

    cJSON *record = NULL;

    cJSON_ArrayForEach(record, records) {
        cJSON *record_copy =
            cJSON_Duplicate(record, 1);

        if (record_copy == NULL) {
            continue;
        }

        if (record_is_overdue(record)) {
            cJSON_ReplaceItemInObjectCaseSensitive(
                record_copy,
                "status",
                cJSON_CreateString("OVERDUE")
            );
        }

        cJSON *record_user_id =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "user_id"
            );

        cJSON *record_book_id =
            cJSON_GetObjectItemCaseSensitive(
                record,
                "book_id"
            );

        const char *username = "Unknown user";
        const char *reader_name = "Unknown user";
        const char *book_title = "Unknown book";

        if (cJSON_IsNumber(record_user_id)) {
            cJSON *user = NULL;

            cJSON_ArrayForEach(user, users) {
                cJSON *user_id =
                    cJSON_GetObjectItemCaseSensitive(
                        user,
                        "id"
                    );

                if (cJSON_IsNumber(user_id) &&
                    user_id->valueint ==
                        record_user_id->valueint) {
                    cJSON *user_username =
                        cJSON_GetObjectItemCaseSensitive(
                            user,
                            "username"
                        );

                    cJSON *user_name =
                        cJSON_GetObjectItemCaseSensitive(
                            user,
                            "name"
                        );

                    if (cJSON_IsString(user_username)) {
                        username =
                            user_username->valuestring;
                    }

                    if (cJSON_IsString(user_name)) {
                        reader_name =
                            user_name->valuestring;
                    }

                    break;
                }
            }
        }

        if (cJSON_IsNumber(record_book_id)) {
            cJSON *book = NULL;

            cJSON_ArrayForEach(book, books) {
                cJSON *book_id =
                    cJSON_GetObjectItemCaseSensitive(
                        book,
                        "id"
                    );

                if (cJSON_IsNumber(book_id) &&
                    book_id->valueint ==
                        record_book_id->valueint) {
                    cJSON *title =
                        cJSON_GetObjectItemCaseSensitive(
                            book,
                            "title"
                        );

                    if (cJSON_IsString(title)) {
                        book_title =
                            title->valuestring;
                    }

                    break;
                }
            }
        }

        cJSON_AddStringToObject(
            record_copy,
            "username",
            username
        );

        cJSON_AddStringToObject(
            record_copy,
            "reader_name",
            reader_name
        );

        cJSON_AddStringToObject(
            record_copy,
            "title",
            book_title
        );

        cJSON_AddItemToArray(
            result_records,
            record_copy
        );
    }

    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data == NULL) {
        cJSON_Delete(result_records);
        goto cleanup;
    }

    cJSON_AddNumberToObject(
        response_data,
        "count",
        cJSON_GetArraySize(result_records)
    );

    cJSON_AddItemToObject(
        response_data,
        "records",
        result_records
    );

    response = create_response(
        1,
        request_id,
        "All borrow records retrieved",
        response_data
    );

cleanup:
    cJSON_Delete(users);
    cJSON_Delete(books);
    cJSON_Delete(records);

    pthread_mutex_unlock(&data_mutex);

    if (response == NULL) {
        return create_response(
            0,
            request_id,
            "Cannot create response",
            NULL
        );
    }

    return response;
}

static char *handle_list_users(
    const char *request_id,
    const ClientSession *session
)
{
    if (!is_admin(session)) {
        return create_response(
            0,
            request_id,
            "Administrator permission required",
            NULL
        );
    }

    char *response = NULL;
    cJSON *users = NULL;

    pthread_mutex_lock(&data_mutex);

    users = read_json_array_file(USERS_FILE);

    if (users == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot read users.json",
            NULL
        );

        goto cleanup;
    }

    cJSON *safe_users = cJSON_CreateArray();

    if (safe_users == NULL) {
        goto cleanup;
    }

    cJSON *user = NULL;

    cJSON_ArrayForEach(user, users) {
        cJSON *safe_user = cJSON_CreateObject();

        if (safe_user == NULL) {
            continue;
        }

        cJSON *id =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "id"
            );

        cJSON *username =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "username"
            );

        cJSON *name =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "name"
            );

        cJSON *role =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "role"
            );

        cJSON *enabled =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "enabled"
            );

        if (cJSON_IsNumber(id)) {
            cJSON_AddNumberToObject(
                safe_user,
                "id",
                id->valueint
            );
        }

        if (cJSON_IsString(username)) {
            cJSON_AddStringToObject(
                safe_user,
                "username",
                username->valuestring
            );
        }

        if (cJSON_IsString(name)) {
            cJSON_AddStringToObject(
                safe_user,
                "name",
                name->valuestring
            );
        }

        if (cJSON_IsString(role)) {
            cJSON_AddStringToObject(
                safe_user,
                "role",
                role->valuestring
            );
        }

        cJSON_AddBoolToObject(
            safe_user,
            "enabled",
            cJSON_IsTrue(enabled)
        );

        cJSON_AddItemToArray(
            safe_users,
            safe_user
        );
    }

    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data == NULL) {
        cJSON_Delete(safe_users);
        goto cleanup;
    }

    cJSON_AddNumberToObject(
        response_data,
        "count",
        cJSON_GetArraySize(safe_users)
    );

    /*
     * safe_users 不包含 password 字段，
     * 避免把所有用户密码发送给管理员客户端。
     */
    cJSON_AddItemToObject(
        response_data,
        "users",
        safe_users
    );

    response = create_response(
        1,
        request_id,
        "User list retrieved",
        response_data
    );

cleanup:
    cJSON_Delete(users);
    pthread_mutex_unlock(&data_mutex);

    if (response == NULL) {
        return create_response(
            0,
            request_id,
            "Cannot create response",
            NULL
        );
    }

    return response;
}

static char *handle_set_user_status(
    cJSON *request,
    const char *request_id,
    const ClientSession *session
)
{
    if (!is_admin(session)) {
        return create_response(
            0,
            request_id,
            "Administrator permission required",
            NULL
        );
    }

    cJSON *data =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "data"
        );

    cJSON *user_id_item = NULL;
    cJSON *enabled_item = NULL;

    if (cJSON_IsObject(data)) {
        user_id_item =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "user_id"
            );

        enabled_item =
            cJSON_GetObjectItemCaseSensitive(
                data,
                "enabled"
            );
    }

    if (!cJSON_IsNumber(user_id_item) ||
        user_id_item->valueint <= 0 ||
        !cJSON_IsBool(enabled_item)) {
        return create_response(
            0,
            request_id,
            "Valid user_id and enabled are required",
            NULL
        );
    }

    int target_user_id = user_id_item->valueint;
    int new_enabled = cJSON_IsTrue(enabled_item);

    if (target_user_id == session->user_id &&
        !new_enabled) {
        return create_response(
            0,
            request_id,
            "You cannot disable your own account",
            NULL
        );
    }

    char *response = NULL;
    cJSON *users = NULL;

    pthread_mutex_lock(&data_mutex);

    users = read_json_array_file(USERS_FILE);

    if (users == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot read users.json",
            NULL
        );

        goto cleanup;
    }

    cJSON *target_user = NULL;
    cJSON *user = NULL;

    cJSON_ArrayForEach(user, users) {
        cJSON *id =
            cJSON_GetObjectItemCaseSensitive(
                user,
                "id"
            );

        if (cJSON_IsNumber(id) &&
            id->valueint == target_user_id) {
            target_user = user;
            break;
        }
    }

    if (target_user == NULL) {
        response = create_response(
            0,
            request_id,
            "User not found",
            NULL
        );

        goto cleanup;
    }

    cJSON *role =
        cJSON_GetObjectItemCaseSensitive(
            target_user,
            "role"
        );

    if (cJSON_IsString(role) &&
        strcmp(role->valuestring, "ADMIN") == 0) {
        response = create_response(
            0,
            request_id,
            "Administrator accounts cannot be modified",
            NULL
        );

        goto cleanup;
    }

    cJSON *username =
        cJSON_GetObjectItemCaseSensitive(
            target_user,
            "username"
        );

    cJSON_DeleteItemFromObjectCaseSensitive(
        target_user,
        "enabled"
    );

    cJSON_AddBoolToObject(
        target_user,
        "enabled",
        new_enabled
    );

    if (write_json_file_atomic(
            USERS_FILE,
            users) != 0) {
        response = create_response(
            0,
            request_id,
            "Cannot save users.json",
            NULL
        );

        goto cleanup;
    }

    cJSON *response_data =
        cJSON_CreateObject();

    if (response_data != NULL) {
        cJSON_AddNumberToObject(
            response_data,
            "user_id",
            target_user_id
        );

        if (cJSON_IsString(username)) {
            cJSON_AddStringToObject(
                response_data,
                "username",
                username->valuestring
            );
        }

        cJSON_AddBoolToObject(
            response_data,
            "enabled",
            new_enabled
        );
    }

    response = create_response(
        1,
        request_id,
        new_enabled
            ? "User enabled successfully"
            : "User disabled successfully",
        response_data
    );

cleanup:
    cJSON_Delete(users);
    pthread_mutex_unlock(&data_mutex);

    return response;
}

static char *handle_update_book(
    cJSON *request,
    const char *request_id,
    const ClientSession *session
)
{
    if (!is_admin(session)) {
        return create_response(0, request_id,
            "Administrator permission required", NULL);
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(request, "data");
    cJSON *book_id_item = cJSON_IsObject(data)
        ? cJSON_GetObjectItemCaseSensitive(data, "book_id") : NULL;

    if (!cJSON_IsNumber(book_id_item) || book_id_item->valueint <= 0) {
        return create_response(0, request_id,
            "Valid book_id is required", NULL);
    }

    char *response = NULL;
    cJSON *books = NULL;
    pthread_mutex_lock(&data_mutex);
    books = read_json_array_file(BOOKS_FILE);

    if (books == NULL) {
        response = create_response(0, request_id,
            "Cannot read books.json", NULL);
        goto cleanup;
    }

    cJSON *target = NULL;
    cJSON *book = NULL;
    cJSON_ArrayForEach(book, books) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(book, "id");
        if (cJSON_IsNumber(id) && id->valueint == book_id_item->valueint) {
            target = book;
            break;
        }
    }

    if (target == NULL) {
        response = create_response(0, request_id, "Book not found", NULL);
        goto cleanup;
    }

    int changed = 0;
    const char *fields[] = {
        "title", "author", "publisher", "category", "location"
    };
    size_t field_count = sizeof(fields) / sizeof(fields[0]);

    for (size_t i = 0; i < field_count; i++) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(data, fields[i]);
        if (value == NULL) {
            continue;
        }
        if (!is_nonempty_string(value)) {
            response = create_response(0, request_id,
                "Updated text fields cannot be empty", NULL);
            goto cleanup;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(target, fields[i]);
        cJSON_AddStringToObject(target, fields[i], value->valuestring);
        changed = 1;
    }

    cJSON *new_total = cJSON_GetObjectItemCaseSensitive(data, "total_count");
    if (new_total != NULL) {
        cJSON *old_total = cJSON_GetObjectItemCaseSensitive(
            target, "total_count");
        cJSON *old_available = cJSON_GetObjectItemCaseSensitive(
            target, "available_count");

        if (!cJSON_IsNumber(new_total) || new_total->valueint <= 0 ||
            new_total->valuedouble != (double)new_total->valueint ||
            !cJSON_IsNumber(old_total) || !cJSON_IsNumber(old_available)) {
            response = create_response(0, request_id,
                "Invalid total_count", NULL);
            goto cleanup;
        }

        int borrowed_count = old_total->valueint - old_available->valueint;
        if (borrowed_count < 0 || new_total->valueint < borrowed_count) {
            response = create_response(0, request_id,
                "total_count cannot be less than borrowed copies", NULL);
            goto cleanup;
        }

        cJSON_SetNumberValue(old_total, new_total->valueint);
        cJSON_SetNumberValue(old_available,
            new_total->valueint - borrowed_count);
        changed = 1;
    }

    if (!changed) {
        response = create_response(0, request_id,
            "No book fields were provided", NULL);
        goto cleanup;
    }

    if (write_json_file_atomic(BOOKS_FILE, books) != 0) {
        response = create_response(0, request_id,
            "Cannot save books.json", NULL);
        goto cleanup;
    }

    cJSON *response_data = cJSON_CreateObject();
    if (response_data != NULL) {
        cJSON_AddNumberToObject(response_data, "book_id",
            book_id_item->valueint);
    }
    response = create_response(1, request_id,
        "Book updated successfully", response_data);

cleanup:
    cJSON_Delete(books);
    pthread_mutex_unlock(&data_mutex);
    return response;
}

static char *handle_set_book_status(
    cJSON *request,
    const char *request_id,
    const ClientSession *session
)
{
    if (!is_admin(session)) {
        return create_response(0, request_id,
            "Administrator permission required", NULL);
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(request, "data");
    cJSON *book_id_item = cJSON_IsObject(data)
        ? cJSON_GetObjectItemCaseSensitive(data, "book_id") : NULL;
    cJSON *status_item = cJSON_IsObject(data)
        ? cJSON_GetObjectItemCaseSensitive(data, "status") : NULL;

    if (!cJSON_IsNumber(book_id_item) || book_id_item->valueint <= 0 ||
        !cJSON_IsString(status_item) ||
        (strcmp(status_item->valuestring, "NORMAL") != 0 &&
         strcmp(status_item->valuestring, "DISABLED") != 0)) {
        return create_response(0, request_id,
            "Valid book_id and status are required", NULL);
    }

    char *response = NULL;
    cJSON *books = NULL;
    pthread_mutex_lock(&data_mutex);
    books = read_json_array_file(BOOKS_FILE);

    if (books == NULL) {
        response = create_response(0, request_id,
            "Cannot read books.json", NULL);
        goto cleanup;
    }

    cJSON *target = NULL;
    cJSON *book = NULL;
    cJSON_ArrayForEach(book, books) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(book, "id");
        if (cJSON_IsNumber(id) && id->valueint == book_id_item->valueint) {
            target = book;
            break;
        }
    }

    if (target == NULL) {
        response = create_response(0, request_id, "Book not found", NULL);
        goto cleanup;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(target, "status");
    cJSON_AddStringToObject(target, "status", status_item->valuestring);

    if (write_json_file_atomic(BOOKS_FILE, books) != 0) {
        response = create_response(0, request_id,
            "Cannot save books.json", NULL);
        goto cleanup;
    }

    cJSON *response_data = cJSON_CreateObject();
    if (response_data != NULL) {
        cJSON_AddNumberToObject(response_data, "book_id",
            book_id_item->valueint);
        cJSON_AddStringToObject(response_data, "status",
            status_item->valuestring);
    }
    response = create_response(1, request_id,
        "Book status updated successfully", response_data);

cleanup:
    cJSON_Delete(books);
    pthread_mutex_unlock(&data_mutex);
    return response;
}

static int should_audit_action(const char *action)
{
    const char *audited[] = {
        "LOGIN", "LOGOUT", "BORROW_BOOK", "RETURN_BOOK", "RENEW_BOOK",
        "ADD_BOOK", "UPDATE_BOOK", "SET_BOOK_STATUS", "ADD_USER",
        "SET_USER_STATUS", "CHANGE_CREDENTIALS"
    };
    size_t count = sizeof(audited) / sizeof(audited[0]);

    for (size_t i = 0; i < count; i++) {
        if (strcmp(action, audited[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void append_operation_log(
    const ClientSession *session,
    const char *action,
    int success,
    const char *message
)
{
    pthread_mutex_lock(&data_mutex);
    cJSON *logs = read_json_array_file(OPERATION_LOGS_FILE);
    if (logs == NULL) {
        logs = cJSON_CreateArray();
    }

    if (logs != NULL) {
        int next_id = 1;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, logs) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "log_id");
            if (cJSON_IsNumber(id) && id->valueint >= next_id) {
                next_id = id->valueint + 1;
            }
        }

        char time_text[32];
        format_time_string(time(NULL), time_text, sizeof(time_text));
        cJSON *log = cJSON_CreateObject();
        if (log != NULL) {
            cJSON_AddNumberToObject(log, "log_id", next_id);
            cJSON_AddNumberToObject(log, "user_id",
                session != NULL ? session->user_id : 0);
            cJSON_AddStringToObject(log, "username",
                session != NULL && session->username[0] != '\0'
                    ? session->username : "anonymous");
            cJSON_AddStringToObject(log, "role",
                session != NULL && session->role[0] != '\0'
                    ? session->role : "NONE");
            cJSON_AddStringToObject(log, "action", action);
            cJSON_AddBoolToObject(log, "success", success);
            cJSON_AddStringToObject(log, "message",
                message != NULL ? message : "");
            cJSON_AddStringToObject(log, "time", time_text);
            cJSON_AddItemToArray(logs, log);
            write_json_file_atomic(OPERATION_LOGS_FILE, logs);
        }
    }

    cJSON_Delete(logs);
    pthread_mutex_unlock(&data_mutex);
}

static char *handle_list_logs(
    const char *request_id,
    const ClientSession *session
)
{
    if (!is_admin(session)) {
        return create_response(0, request_id,
            "Administrator permission required", NULL);
    }

    pthread_mutex_lock(&data_mutex);
    cJSON *logs = read_json_array_file(OPERATION_LOGS_FILE);
    if (logs == NULL) {
        logs = cJSON_CreateArray();
    }

    if (logs == NULL) {
        pthread_mutex_unlock(&data_mutex);
        return create_response(0, request_id,
            "Cannot read operation logs", NULL);
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        cJSON_Delete(logs);
        pthread_mutex_unlock(&data_mutex);
        return create_response(0, request_id,
            "Cannot create response", NULL);
    }

    cJSON_AddNumberToObject(data, "count", cJSON_GetArraySize(logs));
    cJSON_AddItemToObject(data, "logs", logs);
    char *response = create_response(1, request_id,
        "Operation logs retrieved", data);
    pthread_mutex_unlock(&data_mutex);
    return response;
}

static int parse_time_string(
    const char *text,
    time_t *result
)
{
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;

    if (text == NULL || result == NULL) {
        return 0;
    }

    if (sscanf(
            text,
            "%d-%d-%d %d:%d:%d",
            &year,
            &month,
            &day,
            &hour,
            &minute,
            &second) != 6) {
        return 0;
    }

    struct tm value = {0};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    value.tm_isdst = -1;

    time_t converted = mktime(&value);

    if (converted == (time_t)-1) {
        return 0;
    }

    *result = converted;
    return 1;
}

static char *handle_renew_book(
    cJSON *request,
    const char *request_id,
    const ClientSession *session
)
{
    cJSON *data = cJSON_GetObjectItemCaseSensitive(
        request,
        "data"
    );
    cJSON *record_id_item = NULL;

    if (cJSON_IsObject(data)) {
        record_id_item = cJSON_GetObjectItemCaseSensitive(
            data,
            "record_id"
        );
    }

    if (!cJSON_IsNumber(record_id_item) ||
        record_id_item->valueint <= 0) {
        return create_response(
            0,
            request_id,
            "Valid record_id is required",
            NULL
        );
    }

    int requested_record_id = record_id_item->valueint;
    char *response = NULL;
    cJSON *records = NULL;

    pthread_mutex_lock(&data_mutex);

    records = read_json_array_file(BORROW_RECORDS_FILE);

    if (records == NULL) {
        response = create_response(
            0,
            request_id,
            "Cannot read borrow records",
            NULL
        );
        goto cleanup;
    }

    cJSON *target_record = NULL;
    cJSON *record = NULL;

    cJSON_ArrayForEach(record, records) {
        cJSON *record_id = cJSON_GetObjectItemCaseSensitive(
            record,
            "record_id"
        );
        cJSON *user_id = cJSON_GetObjectItemCaseSensitive(
            record,
            "user_id"
        );

        if (cJSON_IsNumber(record_id) &&
            cJSON_IsNumber(user_id) &&
            record_id->valueint == requested_record_id &&
            user_id->valueint == session->user_id) {
            target_record = record;
            break;
        }
    }

    if (target_record == NULL) {
        response = create_response(
            0,
            request_id,
            "Borrow record not found",
            NULL
        );
        goto cleanup;
    }

    cJSON *status = cJSON_GetObjectItemCaseSensitive(
        target_record,
        "status"
    );
    cJSON *due_time_item = cJSON_GetObjectItemCaseSensitive(
        target_record,
        "due_time"
    );
    cJSON *renew_count_item = cJSON_GetObjectItemCaseSensitive(
        target_record,
        "renew_count"
    );

    if (!cJSON_IsString(status) ||
        strcmp(status->valuestring, "BORROWED") != 0) {
        response = create_response(
            0,
            request_id,
            "Only borrowed books can be renewed",
            NULL
        );
        goto cleanup;
    }

    if (!cJSON_IsString(due_time_item) ||
        !cJSON_IsNumber(renew_count_item)) {
        response = create_response(
            0,
            request_id,
            "Invalid borrow record",
            NULL
        );
        goto cleanup;
    }

    if (renew_count_item->valueint >= MAX_RENEW_COUNT) {
        response = create_response(
            0,
            request_id,
            "Renewal limit reached",
            NULL
        );
        goto cleanup;
    }

    time_t old_due_time;

    if (!parse_time_string(
            due_time_item->valuestring,
            &old_due_time)) {
        response = create_response(
            0,
            request_id,
            "Invalid due time",
            NULL
        );
        goto cleanup;
    }

    if (time(NULL) > old_due_time) {
        response = create_response(
            0,
            request_id,
            "Overdue books cannot be renewed",
            NULL
        );
        goto cleanup;
    }

    time_t new_due_time = old_due_time +
        (time_t)BORROW_DAYS * 24 * 60 * 60;
    char new_due_time_text[32];

    format_time_string(
        new_due_time,
        new_due_time_text,
        sizeof(new_due_time_text)
    );

    cJSON_ReplaceItemInObjectCaseSensitive(
        target_record,
        "due_time",
        cJSON_CreateString(new_due_time_text)
    );
    cJSON_SetNumberValue(
        renew_count_item,
        renew_count_item->valueint + 1
    );

    if (write_json_file_atomic(
            BORROW_RECORDS_FILE,
            records) != 0) {
        response = create_response(
            0,
            request_id,
            "Cannot save renewal",
            NULL
        );
        goto cleanup;
    }

    cJSON *response_data = cJSON_CreateObject();

    if (response_data != NULL) {
        cJSON_AddNumberToObject(
            response_data,
            "record_id",
            requested_record_id
        );
        cJSON_AddStringToObject(
            response_data,
            "due_time",
            new_due_time_text
        );
        cJSON_AddNumberToObject(
            response_data,
            "renew_count",
            renew_count_item->valueint
        );
    }

    response = create_response(
        1,
        request_id,
        "Book renewed successfully",
        response_data
    );

cleanup:
    cJSON_Delete(records);
    pthread_mutex_unlock(&data_mutex);
    return response;
}

static char *handle_change_credentials(
    cJSON *request,
    const char *request_id,
    ClientSession *session
)
{
    if (strcmp(session->role, "READER") != 0) {
        return create_response(
            0,
            request_id,
            "This operation is available to readers only",
            NULL
        );
    }

    cJSON *data = cJSON_GetObjectItemCaseSensitive(request, "data");
    cJSON *current_password = cJSON_IsObject(data)
        ? cJSON_GetObjectItemCaseSensitive(data, "current_password") : NULL;
    cJSON *new_username = cJSON_IsObject(data)
        ? cJSON_GetObjectItemCaseSensitive(data, "new_username") : NULL;
    cJSON *new_password = cJSON_IsObject(data)
        ? cJSON_GetObjectItemCaseSensitive(data, "new_password") : NULL;

    if (!is_nonempty_string(current_password)) {
        return create_response(
            0,
            request_id,
            "Current password is required",
            NULL
        );
    }

    if (new_username != NULL && !is_nonempty_string(new_username)) {
        return create_response(0, request_id,
            "New username cannot be empty", NULL);
    }
    if (new_password != NULL && !is_nonempty_string(new_password)) {
        return create_response(0, request_id,
            "New password cannot be empty", NULL);
    }
    if (new_username == NULL && new_password == NULL) {
        return create_response(0, request_id,
            "New username or password is required", NULL);
    }
    if ((new_username != NULL &&
         strlen(new_username->valuestring) >= USERNAME_SIZE) ||
        (new_password != NULL &&
         strlen(new_password->valuestring) > 128)) {
        return create_response(0, request_id,
            "New credentials are too long", NULL);
    }

    char *response = NULL;
    cJSON *users = NULL;
    pthread_mutex_lock(&data_mutex);
    users = read_json_array_file(USERS_FILE);

    if (users == NULL) {
        response = create_response(0, request_id,
            "Cannot read users.json", NULL);
        goto cleanup;
    }

    cJSON *target_user = NULL;
    cJSON *user = NULL;
    cJSON_ArrayForEach(user, users) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(user, "id");
        if (cJSON_IsNumber(id) && id->valueint == session->user_id) {
            target_user = user;
            break;
        }
    }

    if (target_user == NULL) {
        response = create_response(0, request_id,
            "Current user not found", NULL);
        goto cleanup;
    }

    cJSON *stored_password = cJSON_GetObjectItemCaseSensitive(
        target_user,
        "password"
    );
    if (!cJSON_IsString(stored_password) ||
        strcmp(stored_password->valuestring,
            current_password->valuestring) != 0) {
        response = create_response(0, request_id,
            "Current password is incorrect", NULL);
        goto cleanup;
    }

    if (new_username != NULL) {
        cJSON *other = NULL;
        cJSON_ArrayForEach(other, users) {
            cJSON *other_id = cJSON_GetObjectItemCaseSensitive(other, "id");
            cJSON *other_username = cJSON_GetObjectItemCaseSensitive(
                other,
                "username"
            );
            if (cJSON_IsNumber(other_id) &&
                other_id->valueint != session->user_id &&
                cJSON_IsString(other_username) &&
                strcmp(other_username->valuestring,
                    new_username->valuestring) == 0) {
                response = create_response(0, request_id,
                    "Username already exists", NULL);
                goto cleanup;
            }
        }

        cJSON_DeleteItemFromObjectCaseSensitive(target_user, "username");
        cJSON_AddStringToObject(
            target_user,
            "username",
            new_username->valuestring
        );
    }

    if (new_password != NULL) {
        cJSON_DeleteItemFromObjectCaseSensitive(target_user, "password");
        cJSON_AddStringToObject(
            target_user,
            "password",
            new_password->valuestring
        );
    }

    if (write_json_file_atomic(USERS_FILE, users) != 0) {
        response = create_response(0, request_id,
            "Cannot save users.json", NULL);
        goto cleanup;
    }

    if (new_username != NULL) {
        snprintf(
            session->username,
            sizeof(session->username),
            "%s",
            new_username->valuestring
        );
    }

    cJSON *response_data = cJSON_CreateObject();
    if (response_data != NULL) {
        cJSON_AddNumberToObject(
            response_data,
            "user_id",
            session->user_id
        );
        cJSON_AddStringToObject(
            response_data,
            "username",
            session->username
        );
    }

    response = create_response(
        1,
        request_id,
        "Credentials updated successfully",
        response_data
    );

cleanup:
    cJSON_Delete(users);
    pthread_mutex_unlock(&data_mutex);
    return response;
}

static int reconcile_inventory(void)
{
    int result = 0;
    int changed = 0;
    pthread_mutex_lock(&data_mutex);

    cJSON *books = read_json_array_file(BOOKS_FILE);
    cJSON *records = read_json_array_file(BORROW_RECORDS_FILE);

    if (books == NULL || records == NULL) {
        cJSON_Delete(books);
        cJSON_Delete(records);
        pthread_mutex_unlock(&data_mutex);
        return -1;
    }

    cJSON *book = NULL;
    cJSON_ArrayForEach(book, books) {
        cJSON *book_id = cJSON_GetObjectItemCaseSensitive(book, "id");
        cJSON *total = cJSON_GetObjectItemCaseSensitive(book, "total_count");
        cJSON *available = cJSON_GetObjectItemCaseSensitive(
            book, "available_count");

        if (!cJSON_IsNumber(book_id) || !cJSON_IsNumber(total) ||
            !cJSON_IsNumber(available) || total->valueint < 0) {
            continue;
        }

        int borrowed = 0;
        cJSON *record = NULL;
        cJSON_ArrayForEach(record, records) {
            cJSON *record_book_id = cJSON_GetObjectItemCaseSensitive(
                record, "book_id");
            if (cJSON_IsNumber(record_book_id) &&
                record_book_id->valueint == book_id->valueint &&
                record_is_active(record)) {
                borrowed++;
            }
        }

        int expected = total->valueint - borrowed;
        if (expected < 0) {
            fprintf(stderr,
                "Inventory warning: book %d has more active loans than copies.\n",
                book_id->valueint);
            expected = 0;
        }

        if (available->valueint != expected) {
            printf(
                "Inventory repaired: book %d available_count %d -> %d\n",
                book_id->valueint,
                available->valueint,
                expected
            );
            cJSON_SetNumberValue(available, expected);
            changed = 1;
        }
    }

    if (changed && write_json_file_atomic(BOOKS_FILE, books) != 0) {
        result = -1;
    }

    cJSON_Delete(books);
    cJSON_Delete(records);
    pthread_mutex_unlock(&data_mutex);
    return result;
}

static char *process_request(
    const char *request_text,
    ClientSession *session
)
{
    cJSON *request =
        cJSON_Parse(request_text);

    if (request == NULL) {
        return create_response(
            0,
            NULL,
            "Invalid JSON",
            NULL
        );
    }

    if (!cJSON_IsObject(request)) {
        cJSON_Delete(request);

        return create_response(
            0,
            NULL,
            "JSON request must be an object",
            NULL
        );
    }

    cJSON *request_id_item =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "request_id"
        );

    const char *request_id = NULL;

    if (cJSON_IsString(request_id_item)) {
        request_id =
            request_id_item->valuestring;
    }

    cJSON *action =
        cJSON_GetObjectItemCaseSensitive(
            request,
            "action"
        );

    if (!cJSON_IsString(action)) {
        char *response = create_response(
            0,
            request_id,
            "Missing or invalid action",
            NULL
        );

        cJSON_Delete(request);
        return response;
    }

    ClientSession audit_session = *session;
    char *response = NULL;

    if (strcmp(
            action->valuestring,
            "PING") == 0) {
        /*
         * PING 不要求登录，
         * 用于测试服务器是否在线。
         */
        response = handle_ping(
            request,
            request_id
        );
    } else if (strcmp(
                   action->valuestring,
                   "LOGIN") == 0) {
        response = handle_login(
            request,
            request_id,
            session
        );
    } else if (!session->logged_in) {
        /*
         * 除 PING 和 LOGIN 外，
         * 其他操作都必须先登录。
         */
        response = create_response(
            0,
            request_id,
            "Please login first",
            NULL
        );
    } else if (strcmp(
               action->valuestring,
               "LIST_BOOKS") == 0) {
    	response = handle_list_books(
        request_id
    	);
    } else if (strcmp(
               action->valuestring,
               "SEARCH_BOOKS") == 0) {
    	response = handle_search_books(
            request,
            request_id
    	);
    } else if (strcmp(
               action->valuestring,
               "BORROW_BOOK") == 0) {
    	response = handle_borrow_book(
            request,
            request_id,
        session
    	);
    } else if (strcmp(
               action->valuestring,
               "MY_BORROWS") == 0) {
    	response = handle_my_borrows(
            request_id,
            session
    	);
    }else if (strcmp(
               action->valuestring,
               "RETURN_BOOK") == 0) {
    	response = handle_return_book(
            request,
            request_id,
            session
    	);
    } else if (strcmp(
               action->valuestring,
               "RENEW_BOOK") == 0) {
        response = handle_renew_book(
            request,
            request_id,
            session
        );
    } else if (strcmp(
               action->valuestring,
               "ADD_BOOK") == 0) {
    	response = handle_add_book(
            request,
            request_id,
            session
    	);
    } else if (strcmp(
               action->valuestring,
               "UPDATE_BOOK") == 0) {
        response = handle_update_book(
            request,
            request_id,
            session
        );
    } else if (strcmp(
               action->valuestring,
               "SET_BOOK_STATUS") == 0) {
        response = handle_set_book_status(
            request,
            request_id,
            session
        );
    } else if (strcmp(
               action->valuestring,
               "ADD_USER") == 0) {
    	response = handle_add_user(
            request,
            request_id,
            session
    	);
    } else if (strcmp(
               action->valuestring,
               "ALL_BORROWS") == 0) {
    	response = handle_all_borrows(
            request_id,
            session
    	);
    }else if (strcmp(
               action->valuestring,
               "LIST_USERS") == 0) {
    	response = handle_list_users(
            request_id,
            session
    	);
    } else if (strcmp(
               action->valuestring,
               "SET_USER_STATUS") == 0) {
    	response = handle_set_user_status(
            request,
            request_id,
            session
    	);
    } else if (strcmp(
               action->valuestring,
               "CHANGE_CREDENTIALS") == 0) {
        response = handle_change_credentials(
            request,
            request_id,
            session
        );
    } else if (strcmp(
               action->valuestring,
               "LIST_LOGS") == 0) {
        response = handle_list_logs(
            request_id,
            session
        );
    } else if (strcmp(
                   action->valuestring,
                   "LOGOUT") == 0) {
        response = handle_logout(
            request_id,
            session
        );
    } else if (strcmp(
                   action->valuestring,
                   "WHO_AM_I") == 0) {
        response = handle_who_am_i(
            request_id,
            session
        );
    } else {
        response = create_response(
            0,
            request_id,
            "Unknown action",
            NULL
        );
    }

    if (response != NULL && should_audit_action(action->valuestring)) {
        cJSON *audit_response = cJSON_Parse(response);
        if (cJSON_IsObject(audit_response)) {
            cJSON *success_item = cJSON_GetObjectItemCaseSensitive(
                audit_response,
                "success"
            );
            cJSON *message_item = cJSON_GetObjectItemCaseSensitive(
                audit_response,
                "message"
            );
            const ClientSession *actor = session->logged_in
                ? session : &audit_session;

            append_operation_log(
                actor,
                action->valuestring,
                cJSON_IsTrue(success_item),
                cJSON_IsString(message_item)
                    ? message_item->valuestring : ""
            );
        }
        cJSON_Delete(audit_response);
    }

    cJSON_Delete(request);
    return response;
}

static void *handle_client(void *argument)
{
    int client_fd = *(int *)argument;
    free(argument);

    ClientSession session = {0};
    char buffer[BUFFER_SIZE];

    printf(
        "[thread %lu] Client connected, fd=%d\n",
        (unsigned long)pthread_self(),
        client_fd
    );

    while (1) {
        ssize_t length = recv_line(
            client_fd,
            buffer,
            sizeof(buffer)
        );

        if (length == 0) {
            printf(
                "[fd=%d] Client disconnected.\n",
                client_fd
            );
            break;
        }

        if (length == -1) {
            perror("recv");
            break;
        }

        if (length == -2) {
            char *response = create_response(
                0,
                NULL,
                "Request is too long",
                NULL
            );

            if (response != NULL) {
                send_line(client_fd, response);
                cJSON_free(response);
            }

            continue;
        }

        if (length == 0) {
            continue;
        }

        cJSON *request_summary = cJSON_Parse(buffer);
        cJSON *summary_action = cJSON_IsObject(request_summary)
            ? cJSON_GetObjectItemCaseSensitive(request_summary, "action")
            : NULL;
        cJSON *summary_id = cJSON_IsObject(request_summary)
            ? cJSON_GetObjectItemCaseSensitive(request_summary, "request_id")
            : NULL;
        printf(
            "[fd=%d] Request: action=%s, request_id=%s\n",
            client_fd,
            cJSON_IsString(summary_action)
                ? summary_action->valuestring : "INVALID",
            cJSON_IsString(summary_id)
                ? summary_id->valuestring : "-"
        );
        cJSON_Delete(request_summary);

        char *response = process_request(
            buffer,
            &session
        );

        if (response == NULL) {
            fprintf(
                stderr,
                "[fd=%d] Failed to create response.\n",
                client_fd
            );
            break;
        }

        printf(
            "[fd=%d] Response: %s\n",
            client_fd,
            response
        );

        if (send_line(client_fd, response) == -1) {
            perror("send");
            cJSON_free(response);
            break;
        }

        cJSON_free(response);
    }

    if (session.logged_in) {
        printf(
            "[fd=%d] User %s logged out because connection closed.\n",
            client_fd,
            session.username
        );
    }

    close(client_fd);
    return NULL;
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
        fprintf(stderr,
            "Invalid LIBRARY_PORT '%s'; using %d.\n",
            value,
            DEFAULT_SERVER_PORT
        );
        return DEFAULT_SERVER_PORT;
    }

    return (int)port;
}

int run_server(void)
{
    int server_fd;
    int reuse = 1;
    int server_port = get_server_port();
    struct sockaddr_in server_addr;

    /*
     * 客户端异常断开时，防止 send() 触发 SIGPIPE
     * 导致整个服务器退出。
     */
    signal(SIGPIPE, SIG_IGN);

    if (reconcile_inventory() != 0) {
        fprintf(
            stderr,
            "Warning: could not validate inventory data.\n"
        );
    }

    server_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)) == -1) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr =
        htonl(INADDR_ANY);
    server_addr.sin_port =
        htons((uint16_t)server_port);

    if (bind(
            server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 10) == -1) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf(
        "Library server listening on port %d...\n",
        server_port
    );

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_length =
            sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &client_addr_length
        );

        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];

        if (inet_ntop(
                AF_INET,
                &client_addr.sin_addr,
                client_ip,
                sizeof(client_ip)) == NULL) {
            snprintf(
                client_ip,
                sizeof(client_ip),
                "unknown"
            );
        }

        printf(
            "Connection from %s:%d, fd=%d\n",
            client_ip,
            ntohs(client_addr.sin_port),
            client_fd
        );

        /*
         * 给线程单独分配 client_fd。
         * 不能直接传递循环内 client_fd 的地址。
         */
        int *thread_client_fd =
            malloc(sizeof(*thread_client_fd));

        if (thread_client_fd == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }

        *thread_client_fd = client_fd;

        pthread_t thread_id;

        int result = pthread_create(
            &thread_id,
            NULL,
            handle_client,
            thread_client_fd
        );

        if (result != 0) {
            fprintf(
                stderr,
                "pthread_create: %s\n",
                strerror(result)
            );

            free(thread_client_fd);
            close(client_fd);
            continue;
        }

        result = pthread_detach(thread_id);

        if (result != 0) {
            fprintf(
                stderr,
                "pthread_detach: %s\n",
                strerror(result)
            );
        }
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
