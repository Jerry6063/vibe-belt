import pygame
import time

pygame.init()
pygame.joystick.init()

count = pygame.joystick.get_count()
print(f"Joysticks found: {count}")

if count == 0:
    print("No controller detected.")
    raise SystemExit

js = pygame.joystick.Joystick(0)
js.init()

print("Controller name:", js.get_name())
print("Axes:", js.get_numaxes())
print("Buttons:", js.get_numbuttons())
print("Move the LEFT stick. Press Ctrl+C to quit.\n")

try:
    while True:
        pygame.event.pump()

        lx = js.get_axis(0)   # left stick X
        ly = js.get_axis(1)   # left stick Y

        print(f"LX={lx:+.3f}  LY={ly:+.3f}", end="\r")
        time.sleep(0.05)

except KeyboardInterrupt:
    print("\nStopped.")

finally:
    pygame.quit()