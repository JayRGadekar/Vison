import requests
import sys
import traceback
try:
    with open('d:/Project/Vison/test_output.txt', 'w') as f:
        f.write("Sending request...\n")
    resp = requests.post('http://127.0.0.1:11435/api/generate', json={'prompt': 'test', 'model': 'runwayml/stable-diffusion-v1-5', 'num_inference_steps': 1, 'width': 256, 'height': 256}, timeout=300)
    with open('d:/Project/Vison/test_output.txt', 'a') as f:
        f.write(f"Response: {resp.status_code} {resp.text}\n")
except Exception as e:
    with open('d:/Project/Vison/test_output.txt', 'a') as f:
        f.write(f"Exception occurred: {type(e).__name__} {e}\n{traceback.format_exc()}\n")