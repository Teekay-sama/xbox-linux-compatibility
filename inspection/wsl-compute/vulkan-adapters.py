import ctypes as C
import json
import sys

UINT = C.c_uint32
POINTER = C.c_void_p

class ApplicationInfo(C.Structure):
    _fields_ = [
        ('sType', UINT), ('pNext', POINTER), ('pApplicationName', C.c_char_p),
        ('applicationVersion', UINT), ('pEngineName', C.c_char_p), ('engineVersion', UINT),
        ('apiVersion', UINT),
    ]

class InstanceInfo(C.Structure):
    _fields_ = [
        ('sType', UINT), ('pNext', POINTER), ('flags', UINT),
        ('pApplicationInfo', C.POINTER(ApplicationInfo)),
        ('enabledLayerCount', UINT), ('ppEnabledLayerNames', POINTER),
        ('enabledExtensionCount', UINT), ('ppEnabledExtensionNames', POINTER),
    ]

class Properties(C.Structure):
    _fields_ = [
        ('apiVersion', UINT), ('driverVersion', UINT), ('vendorID', UINT),
        ('deviceID', UINT), ('deviceType', UINT), ('deviceName', C.c_char * 256),
        ('limits_and_sparse_properties', C.c_ubyte * 2048),
    ]

class Extension(C.Structure):
    _fields_ = [('name', C.c_char * 256), ('version', UINT)]

def checked(result, operation):
    if result:
        sys.exit(f'{operation} failed with VkResult {result}')

vk = C.CDLL('libvulkan.so.1')
vk.vkCreateInstance.argtypes = [C.POINTER(InstanceInfo), POINTER, C.POINTER(POINTER)]
vk.vkDestroyInstance.argtypes = [POINTER, POINTER]
vk.vkDestroyInstance.restype = None
vk.vkEnumeratePhysicalDevices.argtypes = [POINTER, C.POINTER(UINT), C.POINTER(POINTER)]
vk.vkGetPhysicalDeviceProperties.argtypes = [POINTER, C.POINTER(Properties)]
vk.vkGetPhysicalDeviceProperties.restype = None
vk.vkEnumerateDeviceExtensionProperties.argtypes = [POINTER, C.c_char_p, C.POINTER(UINT), C.POINTER(Extension)]
app = ApplicationInfo(apiVersion=(1 << 22) | (3 << 12))
instance = POINTER()
checked(vk.vkCreateInstance(C.byref(InstanceInfo(sType=1, pApplicationInfo=C.pointer(app))), None, C.byref(instance)), 'vkCreateInstance')
try:
    count = UINT()
    checked(vk.vkEnumeratePhysicalDevices(instance, C.byref(count), None), 'vkEnumeratePhysicalDevices')
    handles = (POINTER * count.value)()
    checked(vk.vkEnumeratePhysicalDevices(instance, C.byref(count), handles), 'vkEnumeratePhysicalDevices')
    devices = []
    for handle in handles:
        props = Properties()
        vk.vkGetPhysicalDeviceProperties(handle, C.byref(props))
        count = UINT()
        checked(vk.vkEnumerateDeviceExtensionProperties(handle, None, C.byref(count), None), 'vkEnumerateDeviceExtensionProperties')
        extensions = (Extension * count.value)()
        checked(vk.vkEnumerateDeviceExtensionProperties(handle, None, C.byref(count), extensions), 'vkEnumerateDeviceExtensionProperties')
        names = {extension.name.decode() for extension in extensions}
        devices.append({
            'name': props.deviceName.decode(),
            'vendor_id': f'0x{props.vendorID:04x}',
            'device_id': f'0x{props.deviceID:04x}',
            'type': props.deviceType,
            'vulkan': f'{props.apiVersion >> 22}.{(props.apiVersion >> 12) & 1023}.{props.apiVersion & 4095}',
            'has_push_descriptors': 'VK_KHR_push_descriptor' in names,
            'has_robustness2': 'VK_EXT_robustness2' in names or 'VK_KHR_robustness2' in names,
        })
    print(json.dumps(devices, indent=2))
finally:
    vk.vkDestroyInstance(instance, None)
