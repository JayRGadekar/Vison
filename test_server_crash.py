import os, sys, time, subprocess, requests

print("Killing old python processes...")
os.system("taskkill /F /IM python.exe /T")
time.sleep(2)

print("Starting Uvicorn...")
proc = subprocess.Popen(
    [sys.executable, "-m", "uvicorn", "main:app", "--host", "127.0.0.1", "--port", "11435"],
    cwd="D:/Project/Vison/backend",
    stdout=open("D:/Project/Vison/uvicorn_out.txt", "w"),
    stderr=subprocess.STDOUT
)
time.sleep(5) # wait for server to start

print("Sending request...")
try:
    resp = requests.post("http://127.0.0.1:11435/api/generate", json={
        "prompt": "test", 
        "model": "runwayml/stable-diffusion-v1-5", 
        "num_inference_steps": 1, 
        "width": 128, 
        "height": 128
    }, timeout=30)
    print("Response:", resp.status_code)
except Exception as e:
    print("Request failed:", e)

print("Terminating server...")
proc.terminate()
