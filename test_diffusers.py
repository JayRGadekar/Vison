import sys
sys.path.append('D:/Project/Vison/backend')
from models.image import ImageGenerator

generator = ImageGenerator("runwayml/stable-diffusion-v1-5")
generator.generate(prompt="a test", steps=1)
print("SUCCESS!")