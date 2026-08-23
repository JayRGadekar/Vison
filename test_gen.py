import requests
import time

req = {
    "prompt": "A cool cat",
    "model": "runwayml/stable-diffusion-v1-5",
    "task": "image",
    "width": 512,
    "height": 512,
    "num_inference_steps": 5,
    "guidance_scale": 7.5,
    "seed": -1,
    "upscale_quality": "4x"
}

try:
    print("Sending request...")
    res = requests.post("http://127.0.0.1:11435/api/generate", json=req, timeout=120)
    print("Status Code:", res.status_code)
    try:
        print("JSON:", res.json())
    except:
        print("Text:", res.text[:500])
except Exception as e:
    print("Error:", e)
