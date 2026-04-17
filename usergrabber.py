import requests
import time
import threading
from pathlib import Path
from itertools import cycle

API_URL = "https://api.minecraftservices.com/minecraft/profile/name/{}"
PROXY_TEST_URL = "https://api.minecraftservices.com/minecraft/profile/name/Notch"
TIMEOUT = 10
PROXY_TEST_TIMEOUT = 5
RETRY_DELAY = 2
TOKEN_INVALID_COOLDOWN = 60
RELOAD_INTERVAL = 30
MAX_CONSECUTIVE_FAILURES = 5

class NameSniper:
    def __init__(self, available_file="available.txt", tokens_file="tokens.txt", proxies_file="proxies.txt", output_file="sniped.txt"):
        self.available_file = Path(available_file)
        self.tokens_file = Path(tokens_file)
        self.proxies_file = Path(proxies_file)
        self.output_file = Path(output_file)
        self.lock = threading.Lock()
        self.usernames = []
        self.tokens = []
        self.proxies = []
        self.invalid_tokens = {}
        self.successful_names = set()
        self.failure_counts = {}
        self.bad_proxies = set()
        self.token_cycle = None
        self.proxy_cycle = None
        self.running = True
        self.last_reload = 0.0
        self._load_successful()

    def _read_lines(self, filepath):
        if not filepath.exists():
            return []
        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                return [line.strip() for line in f if line.strip()]
        except:
            return []

    def _load_successful(self):
        if self.output_file.exists():
            try:
                with open(self.output_file, 'r', encoding='utf-8') as f:
                    for line in f:
                        if ':' in line:
                            self.successful_names.add(line.split(':', 1)[0].strip())
            except:
                pass

    def reload_files(self):
        with self.lock:
            new_usernames = self._read_lines(self.available_file)
            new_tokens = self._read_lines(self.tokens_file)
            new_proxies = self._read_lines(self.proxies_file)
            self.usernames = [u for u in new_usernames if u not in self.successful_names]
            self.tokens = new_tokens
            self.proxies = new_proxies
            self.bad_proxies.clear()
            self.token_cycle = cycle(self.tokens) if self.tokens else None
            self.proxy_cycle = cycle(self.proxies) if self.proxies else None
            self.last_reload = time.time()

    def _parse_proxy(self, proxy_str):
        if not proxy_str:
            return {}
        if '://' not in proxy_str:
            proxy_str = f"http://{proxy_str}"
        return {"http": proxy_str, "https": proxy_str}

    def test_proxy(self, proxy_str):
        if not proxy_str:
            return True
        proxies = self._parse_proxy(proxy_str)
        try:
            requests.get(PROXY_TEST_URL, proxies=proxies, timeout=PROXY_TEST_TIMEOUT)
            return True
        except:
            return False

    def get_working_proxy(self):
        if not self.proxy_cycle:
            return None
        attempts = 0
        max_attempts = len(self.proxies) * 2
        while attempts < max_attempts:
            proxy = next(self.proxy_cycle)
            if proxy in self.bad_proxies:
                attempts += 1
                continue
            if self.test_proxy(proxy):
                return proxy
            self.bad_proxies.add(proxy)
            attempts += 1
        return None

    def _get_next_token(self):
        if not self.token_cycle:
            return None
        now = time.time()
        for _ in range(len(self.tokens)):
            token = next(self.token_cycle)
            if now >= self.invalid_tokens.get(token, 0):
                return token
        return None

    def mark_token_invalid(self, token):
        with self.lock:
            self.invalid_tokens[token] = time.time() + TOKEN_INVALID_COOLDOWN

    def change_name(self, new_name, token, proxy):
        headers = {"Authorization": f"Bearer {token}"}
        url = API_URL.format(new_name)
        proxies = self._parse_proxy(proxy) if proxy else {}
        try:
            response = requests.put(url, headers=headers, proxies=proxies, timeout=TIMEOUT)
            return response.status_code, response.text
        except:
            return -1, ""

    def record_success(self, name, token):
        with self.lock:
            self.successful_names.add(name)
            with open(self.output_file, 'a', encoding='utf-8') as f:
                f.write(f"{name}:{token}\n")
            print(f"snipel :{name}:{token}")

    def run_forever(self):
        self.reload_files()
        while self.running:
            try:
                if time.time() - self.last_reload >= RELOAD_INTERVAL:
                    self.reload_files()
                if not self.usernames or not self.tokens:
                    time.sleep(RELOAD_INTERVAL)
                    continue
                with self.lock:
                    usernames_snapshot = list(self.usernames)
                for username in usernames_snapshot:
                    if username in self.successful_names:
                        continue
                    token = self._get_next_token()
                    if not token:
                        time.sleep(2)
                        continue
                    if self.proxies:
                        proxy = self.get_working_proxy()
                        if not proxy:
                            time.sleep(5)
                            continue
                    else:
                        proxy = None
                    status, _ = self.change_name(username, token, proxy)
                    if status in (200, 204):
                        self.record_success(username, token)
                        with self.lock:
                            self.failure_counts.pop(username, None)
                            if username in self.usernames:
                                self.usernames.remove(username)
                    elif status == 409:
                        with self.lock:
                            self.failure_counts[username] = 0
                        time.sleep(RETRY_DELAY)
                    elif status in (401, 403):
                        self.mark_token_invalid(token)
                    elif status == 429:
                        time.sleep(10)
                    else:
                        if proxy:
                            self.bad_proxies.add(proxy)
                        with self.lock:
                            self.failure_counts[username] = self.failure_counts.get(username, 0) + 1
                            if self.failure_counts[username] >= MAX_CONSECUTIVE_FAILURES:
                                if username in self.usernames:
                                    self.usernames.remove(username)
                        time.sleep(RETRY_DELAY)
                time.sleep(0.5)
            except KeyboardInterrupt:
                self.running = False
            except:
                time.sleep(5)

if __name__ == "__main__":
    NameSniper().run_forever()