/* Read-only system API probe, run as the already-authorized ADB shell.
 * Does not attach/publish/subscribe, change settings, or start a receiver. */
import java.lang.reflect.*;
import java.util.*;

public final class FrameworkProbe {
    private static String cause(Throwable t) {
        while (t instanceof InvocationTargetException && t.getCause() != null) t = t.getCause();
        return t.getClass().getName() + ": " + t.getMessage();
    }
    private static void read(Object target, String method) {
        try {
            Object value = target.getClass().getMethod(method).invoke(target);
            System.out.println(method + "=" + String.valueOf(value));
        } catch (Throwable t) { System.out.println(method + "=UNKNOWN (" + cause(t) + ")"); }
    }
    public static void main(String[] args) throws Exception {
        System.out.println("FOLD5_FRAMEWORK_PROBE=1\nmode=read_only");
        Class<?> process = Class.forName("android.os.Process");
        System.out.println("uid=" + process.getMethod("myUid").invoke(null));
        // System APIs which expect a Looper can inspect characteristics here.
        Class<?> looper = Class.forName("android.os.Looper");
        if (looper.getMethod("myLooper").invoke(null) == null) looper.getMethod("prepareMainLooper").invoke(null);
        Class<?> thread = Class.forName("android.app.ActivityThread");
        Object at = thread.getMethod("systemMain").invoke(null);
        Object system = thread.getMethod("getSystemContext").invoke(at);
        Class<?> context = Class.forName("android.content.Context");
        Object shell = context.getMethod("createPackageContext", String.class, int.class)
                              .invoke(system, "com.android.shell", 0);
        System.out.println("package=" + context.getMethod("getPackageName").invoke(shell));
        Method permission = context.getMethod("checkSelfPermission", String.class);
        for (String p : new String[]{"MANAGE_WIFI_INTERFACES", "NETWORK_SETTINGS", "NETWORK_FACTORY",
                "CREATE_APP_SPECIFIC_NETWORK", "CONNECTIVITY_USE_RESTRICTED_NETWORKS", "NEARBY_WIFI_DEVICES"}) {
            int result = (Integer)permission.invoke(shell, "android.permission." + p);
            System.out.println("permission." + p + "=" + (result == 0 ? "granted" : "denied"));
        }
        Method getService = context.getMethod("getSystemService", String.class);
        for (String name : new String[]{"wifiaware", "sem_wifi_aware", "sem_wifi"}) {
            System.out.println("== " + name + " ==");
            Object service;
            try { service = getService.invoke(shell, name); }
            catch (Throwable t) { System.out.println("service_error=" + cause(t)); continue; }
            if (service == null) { System.out.println("service=absent"); continue; }
            System.out.println("class=" + service.getClass().getName());
            if (name.equals("wifiaware")) {
                for (String method : new String[]{"isAvailable", "getCharacteristics", "getAvailableAwareResources", "isDeviceAttached", "isInstantCommunicationModeEnabled"}) read(service, method);
                try {
                    Object c = service.getClass().getMethod("getCharacteristics").invoke(service);
                    if (c != null) {
                        for (String method : new String[]{"getMaxServiceNameLength", "getMaxServiceSpecificInfoLength", "getMaxMatchFilterLength", "getSupportedCipherSuites", "isInstantCommunicationModeSupported", "isAwarePairingSupported", "isSuspensionSupported", "isSetChannelOnDataPathSupported"}) read(c, method);
                    }
                } catch (Throwable t) { System.out.println("characteristics_error=" + cause(t)); }
            }
            if (name.equals("sem_wifi")) {
                read(service, "isAirdropInterfaceActive");
                read(service, "getWifiSupportedFeatureSet");
            }
            if (name.equals("sem_wifi_aware")) {
                read(service, "isVendorNanServiceAvailable");
                read(service, "isAwareP2pConcurrencySupported");
            }
            // Report API names only: do not execute undocumented vendor operations.
            TreeSet<String> methods = new TreeSet<String>();
            for (Method m : service.getClass().getMethods()) {
                String lower = m.getName().toLowerCase(Locale.ROOT);
                if (lower.matches(".*(airdrop|awdl|mosey|wonder|nan|aware|supportedfeatures|monitor).*")) methods.add(m.toGenericString());
            }
            for (String m : methods) System.out.println("api=" + m);
        }
    }
}
