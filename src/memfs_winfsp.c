#include "memfs_winfsp.h"

#include <assert.h>
#include <stdio.h>

static MemfsWinFsp* memfs_instance(FSP_FILE_SYSTEM* file_system) {
	return (MemfsWinFsp*)file_system->UserContext;
}

static NTSTATUS memfs_status(MemfsResult result) {
	switch (result) {
	case MEMFS_OK:
		return STATUS_SUCCESS;
	case MEMFS_ERR_NOT_FOUND:
		return STATUS_OBJECT_NAME_NOT_FOUND;
	case MEMFS_ERR_PATH_NOT_FOUND:
		return STATUS_OBJECT_PATH_NOT_FOUND;
	case MEMFS_ERR_EXISTS:
		return STATUS_OBJECT_NAME_COLLISION;
	case MEMFS_ERR_NOT_DIRECTORY:
		return STATUS_NOT_A_DIRECTORY;
	case MEMFS_ERR_IS_DIRECTORY:
		return STATUS_FILE_IS_A_DIRECTORY;
	case MEMFS_ERR_NOT_EMPTY:
		return STATUS_DIRECTORY_NOT_EMPTY;
	case MEMFS_ERR_NO_SPACE:
		return STATUS_DISK_FULL;
	case MEMFS_ERR_NO_MEMORY:
		return STATUS_INSUFFICIENT_RESOURCES;
	case MEMFS_ERR_ACCESS:
		return STATUS_ACCESS_DENIED;
	case MEMFS_ERR_INVALID:
	default:
		return STATUS_INVALID_PARAMETER;
	}
}

static uint32_t memfs_normalize_attributes(MemfsNode* node, uint32_t attributes) {
	if (memfs_node_is_directory(node)) {
		attributes |= FILE_ATTRIBUTE_DIRECTORY;
		attributes &= ~FILE_ATTRIBUTE_NORMAL;
	} else {
		attributes &= ~FILE_ATTRIBUTE_DIRECTORY;
		if (attributes == 0)
			attributes = FILE_ATTRIBUTE_NORMAL;
		else if (attributes & ~FILE_ATTRIBUTE_NORMAL)
			attributes &= ~FILE_ATTRIBUTE_NORMAL;
	}
	return attributes;
}

static void memfs_fill_file_info(MemfsNode* node, FSP_FSCTL_FILE_INFO* file_info) {
	memset(file_info, 0, sizeof(*file_info));

	file_info->FileAttributes = node->attributes;
	file_info->AllocationSize = node->allocation_size;
	file_info->FileSize = node->file_size;
	file_info->CreationTime = node->creation_time;
	file_info->LastAccessTime = node->last_access_time;
	file_info->LastWriteTime = node->last_write_time;
	file_info->ChangeTime = node->change_time;
	file_info->IndexNumber = node->index_number;
}

static void memfs_fill_volume_info(Memfs* fs, FSP_FSCTL_VOLUME_INFO* volume_info) {
	memset(volume_info, 0, sizeof(*volume_info));

	volume_info->TotalSize = fs->capacity;
	volume_info->FreeSize = fs->used_bytes <= fs->capacity ? fs->capacity - fs->used_bytes : 0;

	volume_info->VolumeLabelLength = fs->volume_label_bytes;
	memcpy(volume_info->VolumeLabel, fs->volume_label, fs->volume_label_bytes);
}

static NTSTATUS memfs_copy_security(MemfsNode* node, PSECURITY_DESCRIPTOR security_descriptor,
									SIZE_T* security_descriptor_size) {
	if (security_descriptor_size == NULL)
		return STATUS_SUCCESS;

	if (node->security_size > *security_descriptor_size) {
		*security_descriptor_size = node->security_size;
		return STATUS_BUFFER_OVERFLOW;
	}

	*security_descriptor_size = node->security_size;
	if (security_descriptor)
		memcpy(security_descriptor, node->security, node->security_size);

	return STATUS_SUCCESS;
}

static BOOLEAN memfs_add_dir_info(MemfsNode* node, const wchar_t* name, void* buffer, ULONG length,
								  PULONG bytes_transferred) {
	uint8_t storage[sizeof(FSP_FSCTL_DIR_INFO) + (MEMFS_MAX_NAME + 1U) * sizeof(wchar_t)];
	FSP_FSCTL_DIR_INFO* dir_info = (FSP_FSCTL_DIR_INFO*)storage;
	size_t name_chars = wcslen(name);
	size_t name_bytes = name_chars * sizeof(wchar_t);

	if (name_chars > MEMFS_MAX_NAME)
		return FALSE;

	memset(storage, 0, sizeof(storage));
	dir_info->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + name_bytes);
	memfs_fill_file_info(node, &dir_info->FileInfo);

	if (name_bytes)
		memcpy(dir_info->FileNameBuf, name, name_bytes);

	return FspFileSystemAddDirInfo(dir_info, buffer, length, bytes_transferred);
}

static MemfsResult memfs_get_destination(Memfs* fs, const wchar_t* path, MemfsNode** parent,
										 wchar_t name[MEMFS_MAX_NAME + 1]) {
	return memfs_lookup_parent(fs, path, parent, name);
}

static NTSTATUS fs_GetVolumeInfo(FSP_FILE_SYSTEM* file_system, FSP_FSCTL_VOLUME_INFO* volume_info) {
	MemfsWinFsp* instance = memfs_instance(file_system);

	memfs_fill_volume_info(instance->store, volume_info);
	return STATUS_SUCCESS;
}

static NTSTATUS fs_SetVolumeLabel(FSP_FILE_SYSTEM* file_system, PWSTR volume_label,
								  FSP_FSCTL_VOLUME_INFO* volume_info) {
	MemfsWinFsp* instance = memfs_instance(file_system);
	Memfs* fs = instance->store;
	size_t chars;

	if (volume_label == NULL)
		return STATUS_INVALID_PARAMETER;

	chars = wcslen(volume_label);
	if (chars > 31)
		return STATUS_INVALID_VOLUME_LABEL;

	memset(fs->volume_label, 0, sizeof(fs->volume_label));
	memcpy(fs->volume_label, volume_label, chars * sizeof(wchar_t));
	fs->volume_label_bytes = (uint16_t)(chars * sizeof(wchar_t));

	memfs_fill_volume_info(fs, volume_info);
	return STATUS_SUCCESS;
}

static NTSTATUS fs_GetSecurityByName(FSP_FILE_SYSTEM* file_system, PWSTR file_name, PUINT32 file_attributes,
									 PSECURITY_DESCRIPTOR security_descriptor, SIZE_T* security_descriptor_size) {
	MemfsWinFsp* instance = memfs_instance(file_system);
	MemfsNode* node;
	MemfsResult result;

	result = memfs_lookup_path(instance->store, file_name, &node);
	if (result != MEMFS_OK)
		return memfs_status(result);

	if (file_attributes)
		*file_attributes = node->attributes;

	return memfs_copy_security(node, security_descriptor, security_descriptor_size);
}

static NTSTATUS fs_Create(FSP_FILE_SYSTEM* file_system, PWSTR file_name, UINT32 create_options, UINT32 granted_access,
						  UINT32 file_attributes, PSECURITY_DESCRIPTOR security_descriptor, UINT64 allocation_size,
						  PVOID* file_context, FSP_FSCTL_FILE_INFO* file_info) {
	MemfsWinFsp* instance = memfs_instance(file_system);
	MemfsNode* parent;
	MemfsNode* node;
	wchar_t name[MEMFS_MAX_NAME + 1];
	bool directory;
	MemfsResult result;

	(void)granted_access;

	result = memfs_get_destination(instance->store, file_name, &parent, name);
	if (result != MEMFS_OK)
		return memfs_status(result);

	if (memfs_dir_lookup(parent, name))
		return STATUS_OBJECT_NAME_COLLISION;

	directory = 0 != (create_options & FILE_DIRECTORY_FILE);

	result = memfs_node_create(instance->store, parent, name, directory, file_attributes, security_descriptor,
							   directory ? 0 : allocation_size, &node);
	if (result != MEMFS_OK)
		return memfs_status(result);

	*file_context = node;
	memfs_fill_file_info(node, file_info);
	return STATUS_SUCCESS;
}

static NTSTATUS fs_Open(FSP_FILE_SYSTEM* file_system, PWSTR file_name, UINT32 create_options, UINT32 granted_access,
						PVOID* file_context, FSP_FSCTL_FILE_INFO* file_info) {
	MemfsWinFsp* instance = memfs_instance(file_system);
	MemfsNode* node;
	MemfsResult result;

	(void)granted_access;

	result = memfs_lookup_path(instance->store, file_name, &node);
	if (result != MEMFS_OK)
		return memfs_status(result);

	if ((create_options & FILE_DIRECTORY_FILE) && !memfs_node_is_directory(node)) {
		return STATUS_NOT_A_DIRECTORY;
	}

	if ((create_options & FILE_NON_DIRECTORY_FILE) && memfs_node_is_directory(node)) {
		return STATUS_FILE_IS_A_DIRECTORY;
	}

	memfs_node_open(node);
	*file_context = node;
	memfs_fill_file_info(node, file_info);
	return STATUS_SUCCESS;
}

static NTSTATUS fs_Overwrite(FSP_FILE_SYSTEM* file_system, PVOID file_context, UINT32 file_attributes,
							 BOOLEAN replace_file_attributes, UINT64 allocation_size, FSP_FSCTL_FILE_INFO* file_info) {
	MemfsNode* node = file_context;
	MemfsResult result;

	(void)file_system;

	if (memfs_node_is_directory(node))
		return STATUS_FILE_IS_A_DIRECTORY;

	result = memfs_node_set_allocation_size(node, allocation_size);
	if (result != MEMFS_OK)
		return memfs_status(result);

	result = memfs_node_set_file_size(node, 0);
	if (result != MEMFS_OK)
		return memfs_status(result);

	if (replace_file_attributes)
		node->attributes = file_attributes;
	else
		node->attributes |= file_attributes;

	node->attributes = memfs_normalize_attributes(node, node->attributes);
	node->attributes |= FILE_ATTRIBUTE_ARCHIVE;
	node->change_time = memfs_now();

	memfs_fill_file_info(node, file_info);
	return STATUS_SUCCESS;
}

static VOID fs_Cleanup(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name, ULONG flags) {
	MemfsNode* node = file_context;
	uint64_t now;

	(void)file_system;
	(void)file_name;

	if (node == NULL)
		return;

	now = memfs_now();

	if (flags & FspCleanupSetArchiveBit)
		node->attributes |= FILE_ATTRIBUTE_ARCHIVE;
	if (flags & FspCleanupSetLastAccessTime)
		node->last_access_time = now;
	if (flags & FspCleanupSetLastWriteTime)
		node->last_write_time = now;
	if (flags & FspCleanupSetChangeTime)
		node->change_time = now;

	if (flags & FspCleanupDelete)
		memfs_node_unlink(node);
}

static VOID fs_Close(FSP_FILE_SYSTEM* file_system, PVOID file_context) {
	(void)file_system;
	memfs_node_close((MemfsNode*)file_context);
}

static NTSTATUS fs_Read(FSP_FILE_SYSTEM* file_system, PVOID file_context, PVOID buffer, UINT64 offset, ULONG length,
						PULONG bytes_transferred) {
	MemfsNode* node = file_context;
	MemfsResult result;
	uint32_t bytes_read = 0;

	(void)file_system;

	if (memfs_node_is_directory(node))
		return STATUS_FILE_IS_A_DIRECTORY;
	if (offset >= node->file_size)
		return STATUS_END_OF_FILE;

	result = memfs_node_read(node, buffer, offset, length, &bytes_read);
	if (result != MEMFS_OK)
		return memfs_status(result);

	*bytes_transferred = bytes_read;
	return STATUS_SUCCESS;
}

static NTSTATUS fs_Write(FSP_FILE_SYSTEM* file_system, PVOID file_context, PVOID buffer, UINT64 offset, ULONG length,
						 BOOLEAN write_to_end_of_file, BOOLEAN constrained_io, PULONG bytes_transferred,
						 FSP_FSCTL_FILE_INFO* file_info) {
	MemfsNode* node = file_context;
	MemfsResult result;
	uint32_t bytes_written = 0;

	(void)file_system;

	if (memfs_node_is_directory(node))
		return STATUS_FILE_IS_A_DIRECTORY;

	result = memfs_node_write(node, buffer, offset, length, !!write_to_end_of_file, !!constrained_io, &bytes_written);
	if (result != MEMFS_OK)
		return memfs_status(result);

	*bytes_transferred = bytes_written;
	memfs_fill_file_info(node, file_info);
	return STATUS_SUCCESS;
}

static NTSTATUS fs_Flush(FSP_FILE_SYSTEM* file_system, PVOID file_context, FSP_FSCTL_FILE_INFO* file_info) {
	(void)file_system;

	if (file_context && file_info)
		memfs_fill_file_info((MemfsNode*)file_context, file_info);

	return STATUS_SUCCESS;
}

static NTSTATUS fs_GetFileInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context, FSP_FSCTL_FILE_INFO* file_info) {
	(void)file_system;
	memfs_fill_file_info((MemfsNode*)file_context, file_info);
	return STATUS_SUCCESS;
}

static NTSTATUS fs_SetBasicInfo(FSP_FILE_SYSTEM* file_system, PVOID file_context, UINT32 file_attributes,
								UINT64 creation_time, UINT64 last_access_time, UINT64 last_write_time,
								UINT64 change_time, FSP_FSCTL_FILE_INFO* file_info) {
	MemfsNode* node = file_context;

	(void)file_system;

	if (file_attributes != INVALID_FILE_ATTRIBUTES) {
		if (file_attributes == 0)
			file_attributes = memfs_node_is_directory(node) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;

		node->attributes = memfs_normalize_attributes(node, file_attributes);
	}

	if (creation_time)
		node->creation_time = creation_time;
	if (last_access_time)
		node->last_access_time = last_access_time;
	if (last_write_time)
		node->last_write_time = last_write_time;
	if (change_time)
		node->change_time = change_time;

	memfs_fill_file_info(node, file_info);
	return STATUS_SUCCESS;
}

static NTSTATUS fs_SetFileSize(FSP_FILE_SYSTEM* file_system, PVOID file_context, UINT64 new_size,
							   BOOLEAN set_allocation_size, FSP_FSCTL_FILE_INFO* file_info) {
	MemfsNode* node = file_context;
	MemfsResult result;

	(void)file_system;

	if (set_allocation_size)
		result = memfs_node_set_allocation_size(node, new_size);
	else
		result = memfs_node_set_file_size(node, new_size);

	if (result != MEMFS_OK)
		return memfs_status(result);

	memfs_fill_file_info(node, file_info);
	return STATUS_SUCCESS;
}

static NTSTATUS fs_CanDelete(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name) {
	MemfsWinFsp* instance = memfs_instance(file_system);
	MemfsNode* node = file_context;

	(void)file_name;

	if (node == instance->store->root)
		return STATUS_ACCESS_DENIED;

	if (node->dir && node->dir->child_count)
		return STATUS_DIRECTORY_NOT_EMPTY;

	return STATUS_SUCCESS;
}

static NTSTATUS fs_Rename(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name, PWSTR new_file_name,
						  BOOLEAN replace_if_exists) {
	MemfsWinFsp* instance = memfs_instance(file_system);
	MemfsNode* node = file_context;
	MemfsNode* new_parent;
	wchar_t new_name[MEMFS_MAX_NAME + 1];
	MemfsResult result;

	(void)file_name;

	result = memfs_get_destination(instance->store, new_file_name, &new_parent, new_name);
	if (result != MEMFS_OK)
		return memfs_status(result);

	result = memfs_node_rename(node, new_parent, new_name, !!replace_if_exists);
	return memfs_status(result);
}

static NTSTATUS fs_GetSecurity(FSP_FILE_SYSTEM* file_system, PVOID file_context,
							   PSECURITY_DESCRIPTOR security_descriptor, SIZE_T* security_descriptor_size) {
	(void)file_system;

	return memfs_copy_security((MemfsNode*)file_context, security_descriptor, security_descriptor_size);
}

static NTSTATUS fs_SetSecurity(FSP_FILE_SYSTEM* file_system, PVOID file_context,
							   SECURITY_INFORMATION security_information,
							   PSECURITY_DESCRIPTOR modification_descriptor) {
	MemfsNode* node = file_context;
	PSECURITY_DESCRIPTOR new_descriptor;
	uint32_t new_size;
	MemfsResult result;
	NTSTATUS status;

	(void)file_system;

	status = FspSetSecurityDescriptor(node->security, security_information, modification_descriptor, &new_descriptor);
	if (!NT_SUCCESS(status))
		return status;

	new_size = GetSecurityDescriptorLength(new_descriptor);
	result = memfs_node_replace_security(node, new_descriptor, new_size);

	FspDeleteSecurityDescriptor(new_descriptor, (NTSTATUS (*)())FspSetSecurityDescriptor);

	return memfs_status(result);
}

static NTSTATUS fs_ReadDirectory(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR pattern, PWSTR marker,
								 PVOID buffer, ULONG length, PULONG bytes_transferred) {
	MemfsWinFsp* instance = memfs_instance(file_system);
	MemfsNode* dir = file_context;
	MemfsNode* node;

	(void)pattern;

	if (!memfs_node_is_directory(dir))
		return STATUS_NOT_A_DIRECTORY;

	*bytes_transferred = 0;

	if (dir != instance->store->root) {
		if (marker == NULL) {
			if (!memfs_add_dir_info(dir, L".", buffer, length, bytes_transferred)) {
				return STATUS_SUCCESS;
			}
		}

		if (marker == NULL || (marker[0] == L'.' && marker[1] == L'\0')) {
			MemfsNode* parent = dir->parent;

			if (!memfs_add_dir_info(parent, L"..", buffer, length, bytes_transferred)) {
				return STATUS_SUCCESS;
			}
			marker = NULL;
		} else if (marker[0] == L'.' && marker[1] == L'.' && marker[2] == L'\0') {
			marker = NULL;
		}
	}

	for (node = memfs_dir_first(dir); node; node = node->sibling_next) {
		if (node->deleted)
			continue;
		if (marker && _wcsicmp(node->name, marker) <= 0)
			continue;

		if (!memfs_add_dir_info(node, node->name, buffer, length, bytes_transferred)) {
			return STATUS_SUCCESS;
		}
	}

	FspFileSystemAddDirInfo(NULL, buffer, length, bytes_transferred);
	return STATUS_SUCCESS;
}

static NTSTATUS fs_GetDirInfoByName(FSP_FILE_SYSTEM* file_system, PVOID file_context, PWSTR file_name,
									FSP_FSCTL_DIR_INFO* dir_info) {
	MemfsWinFsp* instance = memfs_instance(file_system);
	MemfsNode* dir = file_context;
	MemfsNode* node;
	size_t name_chars;
	size_t name_bytes;

	if (!memfs_node_is_directory(dir))
		return STATUS_NOT_A_DIRECTORY;

	if (wcscmp(file_name, L".") == 0)
		node = dir;
	else if (wcscmp(file_name, L"..") == 0)
		node = dir->parent ? dir->parent : dir;
	else
		node = memfs_dir_lookup(dir, file_name);

	if (node == NULL)
		return STATUS_OBJECT_NAME_NOT_FOUND;

	(void)instance;

	name_chars = wcslen(file_name);
	if (name_chars > MEMFS_MAX_NAME)
		return STATUS_OBJECT_NAME_INVALID;

	name_bytes = name_chars * sizeof(wchar_t);
	memset(dir_info, 0, sizeof(*dir_info));
	dir_info->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + name_bytes);
	memfs_fill_file_info(node, &dir_info->FileInfo);

	if (name_bytes)
		memcpy(dir_info->FileNameBuf, file_name, name_bytes);

	return STATUS_SUCCESS;
}

static const FSP_FILE_SYSTEM_INTERFACE g_memfs_interface = {
	.GetVolumeInfo = fs_GetVolumeInfo,
	.SetVolumeLabel = fs_SetVolumeLabel,
	.GetSecurityByName = fs_GetSecurityByName,
	.Create = fs_Create,
	.Open = fs_Open,
	.Overwrite = fs_Overwrite,
	.Cleanup = fs_Cleanup,
	.Close = fs_Close,
	.Read = fs_Read,
	.Write = fs_Write,
	.Flush = fs_Flush,
	.GetFileInfo = fs_GetFileInfo,
	.SetBasicInfo = fs_SetBasicInfo,
	.SetFileSize = fs_SetFileSize,
	.CanDelete = fs_CanDelete,
	.Rename = fs_Rename,
	.GetSecurity = fs_GetSecurity,
	.SetSecurity = fs_SetSecurity,
	.ReadDirectory = fs_ReadDirectory,
	.GetDirInfoByName = fs_GetDirInfoByName,
};

NTSTATUS memfs_winfsp_create(uint64_t capacity, const wchar_t* volume_label, MemfsWinFsp** out_instance) {
	FSP_FSCTL_VOLUME_PARAMS volume_params;
	MemfsWinFsp* instance;
	MemfsResult result;
	NTSTATUS status;

	if (out_instance == NULL)
		return STATUS_INVALID_PARAMETER;

	*out_instance = NULL;

	instance = calloc(1, sizeof(*instance));
	if (instance == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;

	result = memfs_create(capacity, volume_label, &instance->store);
	if (result != MEMFS_OK) {
		free(instance);
		return memfs_status(result);
	}

	memset(&volume_params, 0, sizeof(volume_params));
	volume_params.Version = sizeof(volume_params);
	volume_params.SectorSize = MEMFS_ALLOCATION_UNIT;
	volume_params.SectorsPerAllocationUnit = 1;
	volume_params.MaxComponentLength = MEMFS_MAX_NAME * sizeof(wchar_t);
	volume_params.VolumeCreationTime = memfs_now();
	volume_params.VolumeSerialNumber = (uint32_t)(volume_params.VolumeCreationTime / 10000000ULL);
	volume_params.FileInfoTimeout = 1000;
	volume_params.CaseSensitiveSearch = 0;
	volume_params.CasePreservedNames = 1;
	volume_params.UnicodeOnDisk = 1;
	volume_params.PersistentAcls = 1;
	volume_params.PostCleanupWhenModifiedOnly = 1;
	volume_params.PassQueryDirectoryFileName = 1;
	volume_params.PostDispositionWhenNecessaryOnly = 1;
	volume_params.AllowOpenInKernelMode = 1;
	volume_params.SupportsPosixUnlinkRename = 0;

	wcscpy_s(volume_params.FileSystemName, _countof(volume_params.FileSystemName), L"MEMFS-C");

	status =
		FspFileSystemCreate(L"" FSP_FSCTL_DISK_DEVICE_NAME, &volume_params, &g_memfs_interface, &instance->file_system);
	if (!NT_SUCCESS(status)) {
		memfs_destroy(instance->store);
		free(instance);
		return status;
	}

	instance->file_system->UserContext = instance;
	FspFileSystemSetOperationGuardStrategy(instance->file_system, FSP_FILE_SYSTEM_OPERATION_GUARD_STRATEGY_COARSE);

	*out_instance = instance;
	return STATUS_SUCCESS;
}

NTSTATUS memfs_winfsp_mount(MemfsWinFsp* instance, const wchar_t* mount_point) {
	if (instance == NULL || mount_point == NULL)
		return STATUS_INVALID_PARAMETER;

	return FspFileSystemSetMountPoint(instance->file_system, (PWSTR)mount_point);
}

NTSTATUS memfs_winfsp_start(MemfsWinFsp* instance, uint32_t thread_count) {
	if (instance == NULL)
		return STATUS_INVALID_PARAMETER;

	return FspFileSystemStartDispatcher(instance->file_system, thread_count);
}

void memfs_winfsp_stop(MemfsWinFsp* instance) {
	if (instance && instance->file_system)
		FspFileSystemStopDispatcher(instance->file_system);
}

void memfs_winfsp_destroy(MemfsWinFsp* instance) {
	if (instance == NULL)
		return;

	if (instance->file_system)
		FspFileSystemDelete(instance->file_system);

	memfs_destroy(instance->store);
	free(instance);
}
