import urllib.request
import json
import time
import sys

REPO = "deemike/zmk-mvp-test"

def check_latest_build():
    url = f"https://api.github.com/repos/{REPO}/actions/runs?per_page=1"
    req = urllib.request.Request(url, headers={"User-Agent": "ZMK-Auto-Watcher"})
    try:
        with urllib.request.urlopen(req) as resp:
            data = json.loads(resp.read().decode())
            runs = data.get("workflow_runs", [])
            if not runs:
                print("No workflow runs found.")
                return None
            return runs[0]
    except Exception as e:
        print("API Error:", e)
        return None

def main():
    print("=== ZMK GitHub Actions Auto Watcher ===")
    run = check_latest_build()
    if not run:
        return

    run_id = run["id"]
    commit_msg = run["head_commit"]["message"].split("\n")[0]
    print(f"Tracking Run ID: {run_id}")
    print(f"Commit: {commit_msg}")
    print("Waiting for build to complete...", end="", flush=True)

    while True:
        url = f"https://api.github.com/repos/{REPO}/actions/runs/{run_id}"
        req = urllib.request.Request(url, headers={"User-Agent": "ZMK-Auto-Watcher"})
        try:
            with urllib.request.urlopen(req) as resp:
                data = json.loads(resp.read().decode())
                status = data.get("status")
                conclusion = data.get("conclusion")
                if status == "completed":
                    print(f"\nBuild finished with status: {conclusion}!")
                    html_url = data.get("html_url")
                    print(f"Build page: {html_url}")
                    if conclusion == "success":
                        print("Firmware compiled successfully! You can download it directly from the build page.")
                    else:
                        print("Build failed. Check the logs on the page above.")
                    break
                else:
                    print(".", end="", flush=True)
        except Exception as e:
            pass
        time.sleep(10)

if __name__ == "__main__":
    main()
