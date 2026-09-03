#ifndef NANOMODBUS_CONFIG_H
#define NANOMODBUS_CONFIG_H

/*
 * Product feature selection for the vendored nanoMODBUS library.
 * Keep these definitions identical for nanomodbus.c and every consumer of
 * nanomodbus.h because they affect public structure layouts.
 */
#define NANOMODBUS_CFG_CLIENT_ENABLED       1
#define NANOMODBUS_CFG_SERVER_ENABLED       1

#if (NANOMODBUS_CFG_CLIENT_ENABLED == 0)
#define NMBS_CLIENT_DISABLED
#endif

#if (NANOMODBUS_CFG_SERVER_ENABLED == 0)
#define NMBS_SERVER_DISABLED
#endif

#endif /* NANOMODBUS_CONFIG_H */
