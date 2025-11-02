# ReSTIR TODO

1. Do BSDF based light sampling in the RIS function - this will hopefully solve the issues appearing when sampling very close a bright light source (because BSDF sampling would easily hit that light but direct light sampling likely wouldn't).
   - Reminder: check the ReSTIR course video for more details

2. Maybe consider less candidates after the first ReSTIR bounce (the one that will eventually have spatial reuse)

3. Maybe sample lights (but not points on lights) by distance so that closer lights get sampled more?
