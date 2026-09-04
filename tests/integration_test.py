#!/usr/bin/env python3
import json
import os
import shutil
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER_BINARY = ROOT / "build" / "server"
FIXTURES = ROOT / "tests" / "fixtures"
TEST_PORT = 18081


class ProtocolClient:
    def __init__(self):
        self.socket = socket.create_connection(("127.0.0.1", TEST_PORT), 3)
        self.stream = self.socket.makefile("rwb")
        self.request_number = 1

    def request(self, action, data=None):
        request = {
            "action": action,
            "request_id": "TEST-{}".format(self.request_number),
            "data": data or {},
        }
        self.request_number += 1
        payload = json.dumps(request, ensure_ascii=False).encode("utf-8") + b"\n"
        self.stream.write(payload)
        self.stream.flush()
        line = self.stream.readline()
        if not line:
            raise AssertionError("server closed the connection")
        return json.loads(line.decode("utf-8"))

    def close(self):
        self.stream.close()
        self.socket.close()


def assert_success(response, expected=True):
    actual = response.get("success") is True
    if actual != expected:
        raise AssertionError("unexpected response: {}".format(response))
    return response


def wait_for_server(process):
    deadline = time.time() + 5
    while time.time() < deadline:
        if process.poll() is not None:
            output = process.stdout.read().decode("utf-8", errors="replace")
            raise RuntimeError("server exited early:\n{}".format(output))
        try:
            connection = socket.create_connection(("127.0.0.1", TEST_PORT), 0.2)
            connection.close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("server did not start within 5 seconds")


def login(client, username, password):
    return assert_success(client.request("LOGIN", {
        "username": username,
        "password": password,
    }))


def run_tests():
    with tempfile.TemporaryDirectory(prefix="library-system-test-") as temporary:
        work = Path(temporary)
        shutil.copytree(str(FIXTURES), str(work / "data"))
        environment = os.environ.copy()
        environment["LIBRARY_PORT"] = str(TEST_PORT)

        process = subprocess.Popen(
            [str(SERVER_BINARY)],
            cwd=str(work),
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

        clients = []
        try:
            wait_for_server(process)

            anonymous = ProtocolClient()
            clients.append(anonymous)
            assert_success(anonymous.request("PING", {"message": "hello"}))
            assert_success(anonymous.request("LIST_BOOKS"), expected=False)
            anonymous.close()
            clients.remove(anonymous)

            reader1 = ProtocolClient()
            clients.append(reader1)
            login(reader1, "reader1", "reader123")
            books = assert_success(reader1.request("LIST_BOOKS"))
            assert books["data"]["count"] == 2

            borrowed = assert_success(reader1.request("BORROW_BOOK", {"book_id": 2001}))
            record_id = borrowed["data"]["record_id"]
            assert_success(reader1.request("BORROW_BOOK", {"book_id": 2001}), False)
            assert_success(reader1.request("RENEW_BOOK", {"record_id": record_id}))
            assert_success(reader1.request("RENEW_BOOK", {"record_id": record_id}), False)
            my_records = assert_success(reader1.request("MY_BORROWS"))
            assert my_records["data"]["count"] == 1
            assert_success(reader1.request("RETURN_BOOK", {"record_id": record_id}))
            assert_success(reader1.request("RETURN_BOOK", {"record_id": record_id}), False)
            assert_success(reader1.request("ADD_BOOK", {
                "isbn": "FORBIDDEN",
                "title": "forbidden",
                "author": "x",
                "publisher": "x",
                "category": "x",
                "location": "x",
                "total_count": 1,
            }), False)

            reader2 = ProtocolClient()
            clients.append(reader2)
            login(reader2, "reader2", "reader123")
            barrier = threading.Barrier(2)
            concurrent_results = []
            concurrent_lock = threading.Lock()

            def borrow_last_copy(client):
                barrier.wait()
                result = client.request("BORROW_BOOK", {"book_id": 2002})
                with concurrent_lock:
                    concurrent_results.append(result["success"] is True)

            first = threading.Thread(target=borrow_last_copy, args=(reader1,))
            second = threading.Thread(target=borrow_last_copy, args=(reader2,))
            first.start()
            second.start()
            first.join()
            second.join()
            assert sorted(concurrent_results) == [False, True]

            admin = ProtocolClient()
            clients.append(admin)
            login(admin, "admin", "admin123")
            added = assert_success(admin.request("ADD_BOOK", {
                "isbn": "TEST-ISBN-003",
                "title": "管理员新增图书",
                "author": "测试作者",
                "publisher": "测试出版社",
                "category": "测试分类",
                "location": "T-03",
                "total_count": 2,
            }))
            new_book_id = added["data"]["book_id"]
            assert_success(admin.request("UPDATE_BOOK", {
                "book_id": new_book_id,
                "location": "T-04",
                "total_count": 3,
            }))
            assert_success(admin.request("SET_BOOK_STATUS", {
                "book_id": new_book_id,
                "status": "DISABLED",
            }))
            assert_success(admin.request("LIST_USERS"))
            assert_success(admin.request("ALL_BORROWS"))
            logs = assert_success(admin.request("LIST_LOGS"))
            assert logs["data"]["count"] > 0

            print("PASS: protocol, permissions, borrowing, renewal, return")
            print("PASS: concurrent last-copy protection")
            print("PASS: administrator book/user/log endpoints")
            print("All integration tests passed.")
        finally:
            for client in clients:
                try:
                    client.close()
                except OSError:
                    pass
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)


if __name__ == "__main__":
    run_tests()
