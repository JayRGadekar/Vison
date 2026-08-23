const req = fetch('http://127.0.0.1:11435/api/generate', {
    method: 'POST',
    headers: {
        'Content-Type': 'application/json'
    },
    body: JSON.stringify({
        prompt: 'test',
        model: 'Comfy-Org/Real-ESRGAN_repackaged',
        task: 'image_upscale',
        num_inference_steps: 5,
        upscale_quality: '4x'
    })
});
req.then(r => r.text()).then(console.log).catch(console.error);