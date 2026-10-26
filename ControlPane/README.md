# YMF754 3D Wide Preference Pane (not used in current driver test)

This pane is retained from an earlier experiment. The current driver no longer
publishes the custom 3D Wide control and instead requests the AC'97 rear-depth
setting during analog codec initialization. Do not build or install this pane for
the current hardware test.

It is not part of the kext target. If the toggle is reintroduced later, first
confirm the effect and register the driver-side control; then rebuild this pane
for both Intel architectures and test it on Mac OS X 10.6.8.
