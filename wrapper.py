import subprocess
import shlex

result = subprocess.run(
    ["python", "test_gen.py"],
    capture_output=True,
    text=True
)

with open("test_gen_result.txt", "w") as f:
    f.write("STDOUT:\n")
    f.write(result.stdout)
    f.write("\nSTDERR:\n")
    f.write(result.stderr)
