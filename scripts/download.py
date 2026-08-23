import sys
import os
import urllib.request
import time

def download_file(url, dest):
    cancel_file = dest + ".cancel"
    
    # Headers to pretend to be a normal browser (fixes Hugging Face 403s)
    headers = {
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36'
    }
    
    # Support resuming if file already partially exists
    existing_size = 0
    if os.path.exists(dest):
        existing_size = os.path.getsize(dest)
        
    total_size = 0
    try:
        req = urllib.request.Request(url, headers=headers, method='HEAD')
        with urllib.request.urlopen(req, timeout=30) as response:
            total_size = int(response.headers.get('content-length', 0))
    except Exception as e:
        # HEAD failed — we'll try the GET anyway and won't show progress percentage
        print(f"PROGRESS: 0", flush=True)
        total_size = 0

    # If the file is already fully downloaded
    if total_size > 0 and existing_size == total_size:
        print("PROGRESS: 100.0", flush=True)
        print("DONE", flush=True)
        return

    # If partially downloaded, attempt resume
    if existing_size > 0 and total_size > 0 and existing_size < total_size:
        headers['Range'] = f'bytes={existing_size}-'
        open_mode = 'ab'
        resume_attempted = True
    else:
        existing_size = 0
        open_mode = 'wb'
        resume_attempted = False

    req = urllib.request.Request(url, headers=headers)
    
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            # Guard against resume + 200 (server ignores Range and sends full file)
            # HTTP 206 = Partial Content (resume worked), 200 = full content
            if resume_attempted and response.status == 200:
                # Server didn't honor the Range header — restart from scratch
                existing_size = 0
                open_mode = 'wb'
                print("PROGRESS: 0", flush=True)
            
            # If we didn't get total_size from HEAD, try from GET response
            if total_size == 0:
                content_length = response.headers.get('content-length', '0')
                try:
                    total_size = int(content_length) + existing_size
                except (ValueError, TypeError):
                    total_size = 0

            with open(dest, open_mode) as f:
                downloaded = existing_size
                chunk_size = 1024 * 1024  # 1MB chunks
                
                # Print initial progress
                if total_size > 0:
                    print(f"PROGRESS: {(downloaded / total_size) * 100:.2f}", flush=True)
                else:
                    print(f"PROGRESS: 0", flush=True)
                
                while True:
                    # Check for cancellation
                    if os.path.exists(cancel_file):
                        print("ERROR: Cancelled by user", flush=True)
                        return
                        
                    chunk = response.read(chunk_size)
                    if not chunk:
                        break
                        
                    f.write(chunk)
                    downloaded += len(chunk)
                    
                    if total_size > 0:
                        pct = min((downloaded / total_size) * 100, 100.0)
                        print(f"PROGRESS: {pct:.2f}", flush=True)
                    else:
                        # No total size known — report downloaded MB
                        mb = downloaded / (1024 * 1024)
                        print(f"PROGRESS: {min(mb / 100, 95):.2f}", flush=True)
            
            # Validate downloaded file size
            actual_size = os.path.getsize(dest) if os.path.exists(dest) else 0
            if total_size > 0 and actual_size < total_size:
                print(f"ERROR: Download incomplete — expected {total_size} bytes, got {actual_size} bytes", flush=True)
                return
                        
        print("DONE", flush=True)
        
    except Exception as e:
        print(f"ERROR: {e}", flush=True)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("ERROR: Usage: python download.py <url> <dest>", flush=True)
        sys.exit(1)
        
    url = sys.argv[1]
    dest = sys.argv[2]
    download_file(url, dest)
